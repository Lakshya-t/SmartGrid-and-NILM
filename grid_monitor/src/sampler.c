/*
 * sampler.c
 *
 * ADC1 (voltage, PA0 = IN0) and ADC2 (current, PA1 = IN1) run in dual
 * regular-simultaneous mode. TIM2 TRGO starts every conversion pair at
 * exactly the same interval. DMA2 stream 0 copies the packed result
 * (ADC1 in bits 15..0, ADC2 in bits 31..16) into a circular buffer.
 * The DMA half/full-transfer interrupt hands each 16-sample block to the
 * protection thread, and also runs a very fast instantaneous over-current
 * check so a short circuit is caught within one block (1.56 ms).
 *
 * Zephyr's own ADC and DMA drivers are NOT used (they cannot do this mode),
 * so the STM32 HAL is called directly.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <stm32f4xx_hal.h>
#include "sampler.h"
#include "protect.h"

LOG_MODULE_REGISTER(sampler, LOG_LEVEL_INF);

#define CLIP_LO 6      /* current channel at the rail = clipping = overload */
#define CLIP_HI 4089

K_SEM_DEFINE(sampler_blk_sem, 0, 1);

static ADC_HandleTypeDef hadc1, hadc2;
static DMA_HandleTypeDef hdma_adc1;
static TIM_HandleTypeDef htim2;

static uint32_t dma_buf[2 * BLOCK_SAMPLES] __aligned(4);
static uint32_t ring[RING_SAMPLES];
static volatile uint32_t widx;
static volatile int32_t ofs_i = 2048;
static volatile int32_t peak_cnt = 1900;
static volatile bool fast_en;

static struct sampler_stats stats;
static float fs_actual = FS_NOMINAL_HZ;

/*
 * IIR low-pass filter state for V and I channels.
 * y[n] = y[n-1] + ((x[n] - y[n-1]) >> IIR_SHIFT)
 * With IIR_SHIFT=2: alpha=0.25, effective cutoff ~540 Hz at 10240 sps.
 * Phase shift at 50 Hz ≈ 5° — applied equally to both channels so the
 * relative V-I phase (power factor) is preserved exactly.
 */
#define IIR_SHIFT 2
static int32_t iir_v;   /* voltage filter state (ADC counts × 1) */
static int32_t iir_i;   /* current filter state (ADC counts × 1) */

/* ---------------------------------------------------------------- ISR -- */

static void block_done(const uint32_t *blk)
{
	uint32_t w = widx;
	int32_t ofs = ofs_i;
	int32_t pk = peak_cnt;
	int32_t worst = 0;
	bool trip = false;

	for (uint32_t k = 0; k < BLOCK_SAMPLES; k++) {
		uint32_t s = blk[k];
		int32_t v_raw = (int32_t)(s & 0xFFFFu);
		int32_t c = (int32_t)((s >> 16) & 0xFFFFu);

		/* IIR low-pass: y += (x - y) >> IIR_SHIFT */
		iir_v += (v_raw - iir_v) >> IIR_SHIFT;
		iir_i += (c     - iir_i) >> IIR_SHIFT;

		/* Re-pack filtered samples and store in ring */
		uint32_t filtered = ((uint32_t)(iir_i & 0xFFFFu) << 16)
				  |  ((uint32_t)(iir_v & 0xFFFFu));
		ring[(w + k) & (RING_SAMPLES - 1u)] = filtered;

		int32_t d = iir_i - ofs;
		if (d < 0) {
			d = -d;
		}
		if (d > pk || c < CLIP_LO || c > CLIP_HI) {
			trip = true;
			if (d > worst) {
				worst = d;
			}
		}
	}
	__atomic_store_n(&widx, w + BLOCK_SAMPLES, __ATOMIC_RELEASE);
	stats.blocks++;
	k_sem_give(&sampler_blk_sem);

	if (trip && fast_en) {
		stats.fast_trips++;
		protect_fast_trip(worst);
	}
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
	ARG_UNUSED(hadc);
	block_done(&dma_buf[0]);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
	ARG_UNUSED(hadc);
	block_done(&dma_buf[BLOCK_SAMPLES]);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
	ARG_UNUSED(hadc);
	stats.adc_err++;
}

static void dma_isr(const void *arg)
{
	ARG_UNUSED(arg);
	HAL_DMA_IRQHandler(&hdma_adc1);
}

static void adc_isr(const void *arg)
{
	ARG_UNUSED(arg);
	/* ADC1/ADC2/ADC3 share one interrupt line: serve both of ours */
	HAL_ADC_IRQHandler(&hadc1);
	HAL_ADC_IRQHandler(&hadc2);
}

/* ------------------------------------------------------------- setup -- */

static uint32_t tim2_clock_hz(void)
{
	static const uint8_t shift[8] = { 0, 0, 0, 0, 1, 2, 3, 4 };
	uint32_t hclk = sys_clock_hw_cycles_per_sec();
	uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
	uint32_t pclk1 = hclk >> shift[ppre1];

	/* timers run at 2 x PCLK1 whenever the APB1 prescaler is not 1 */
	return (ppre1 < 4u) ? pclk1 : pclk1 * 2u;
}

int sampler_init(void)
{
	/* ---- clocks ---- */
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_ADC1_CLK_ENABLE();
	__HAL_RCC_ADC2_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();
	__HAL_RCC_TIM2_CLK_ENABLE();

	/* ---- PA0, PA1 analog ---- */
	GPIO_InitTypeDef gi = {0};

	gi.Pin = GPIO_PIN_0 | GPIO_PIN_1;
	gi.Mode = GPIO_MODE_ANALOG;
	gi.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &gi);

	/* ---- TIM2: update event -> TRGO ---- */
	uint32_t tclk = tim2_clock_hz();
	uint32_t period = (uint32_t)((tclk + (uint32_t)(FS_NOMINAL_HZ / 2.0f)) /
				     (uint32_t)FS_NOMINAL_HZ);   /* rounded */

	stats.timer_hz = tclk;
	stats.arr = period - 1u;
	fs_actual = (float)tclk / (float)period;

	htim2.Instance = TIM2;
	htim2.Init.Prescaler = 0;
	htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim2.Init.Period = period - 1u;
	htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	if (HAL_TIM_Base_Init(&htim2) != HAL_OK) {
		return -EIO;
	}
	TIM_MasterConfigTypeDef mc = {0};

	mc.MasterOutputTrigger = TIM_TRGO_UPDATE;
	mc.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &mc) != HAL_OK) {
		return -EIO;
	}

	/* ---- DMA2 stream 0, channel 0 -> ADC1 ---- */
	hdma_adc1.Instance = DMA2_Stream0;
	hdma_adc1.Init.Channel = DMA_CHANNEL_0;
	hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
	hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
	hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
	hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
	hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
	hdma_adc1.Init.Mode = DMA_CIRCULAR;
	hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;
	hdma_adc1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
	if (HAL_DMA_Init(&hdma_adc1) != HAL_OK) {
		return -EIO;
	}

	/* ---- ADC1 (master, voltage) ---- */
	hadc1.Instance = ADC1;
	hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;
	hadc1.Init.ScanConvMode = DISABLE;
	hadc1.Init.ContinuousConvMode = DISABLE;
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
	hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.DMAContinuousRequests = ENABLE;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	__HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);
	if (HAL_ADC_Init(&hadc1) != HAL_OK) {
		return -EIO;
	}
	ADC_ChannelConfTypeDef ch = {0};

	ch.Channel = ADC_CHANNEL_0;
	ch.Rank = 1;
	ch.SamplingTime = ADC_SAMPLETIME_56CYCLES;
	if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) {
		return -EIO;
	}

	/* ---- ADC2 (slave, current): triggered by the master ---- */
	hadc2.Instance = ADC2;
	hadc2.Init = hadc1.Init;
	hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
	hadc2.Init.DMAContinuousRequests = DISABLE;
	if (HAL_ADC_Init(&hadc2) != HAL_OK) {
		return -EIO;
	}
	ch.Channel = ADC_CHANNEL_1;
	if (HAL_ADC_ConfigChannel(&hadc2, &ch) != HAL_OK) {
		return -EIO;
	}

/* ---- dual regular simultaneous mode, one 32-bit DMA word per pair ---- */
	ADC_MultiModeTypeDef mm = {0};

	mm.Mode = ADC_DUALMODE_REGSIMULT;
	mm.DMAAccessMode = ADC_DMAACCESSMODE_2;
	mm.TwoSamplingDelay = ADC_TWOSAMPLINGDELAY_5CYCLES;

	if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &mm) != HAL_OK) {
		return -EIO;
	}

	IRQ_CONNECT(DMA2_Stream0_IRQn, 1, dma_isr, NULL, 0);
	irq_enable(DMA2_Stream0_IRQn);
	IRQ_CONNECT(ADC_IRQn, 2, adc_isr, NULL, 0);
	irq_enable(ADC_IRQn);

	LOG_INF("sampler: TIM2 clock %u Hz, ARR %u, fs %d Hz",
		stats.timer_hz, stats.arr, (int)(fs_actual + 0.5f));
	return 0;
}

int sampler_start(void)
{
	iir_v = 2048;
	iir_i = (ofs_i > 0) ? ofs_i : 2048;
	if (HAL_ADC_Start(&hadc2) != HAL_OK) {
		return -EIO;
	}
	if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, dma_buf, 2 * BLOCK_SAMPLES) != HAL_OK) {
		return -EIO;
	}
	if (HAL_TIM_Base_Start(&htim2) != HAL_OK) {
		return -EIO;
	}
	return 0;
}

/* ------------------------------------------------------------ access -- */

float sampler_fs(void)
{
	return fs_actual;
}

uint32_t sampler_widx(void)
{
	return __atomic_load_n(&widx, __ATOMIC_ACQUIRE);
}

uint32_t sampler_ring_at(uint32_t idx)
{
	return ring[idx & (RING_SAMPLES - 1u)];
}

bool sampler_copy_window(uint16_t *v, uint16_t *i, uint32_t n)
{
	uint32_t e = sampler_widx();

	if (e < n) {
		return false;
	}
	uint32_t s = e - n;

	for (uint32_t k = 0; k < n; k++) {
		uint32_t w = ring[(s + k) & (RING_SAMPLES - 1u)];

		v[k] = (uint16_t)(w & 0xFFFFu);
		i[k] = (uint16_t)(w >> 16);
	}
	/* make sure the writer did not lap the data we just copied */
	return (sampler_widx() - s) <= (RING_SAMPLES - BLOCK_SAMPLES);
}

void sampler_set_ofs_i(int32_t counts)
{
	ofs_i = counts;
}

void sampler_set_peak_counts(int32_t counts)
{
	peak_cnt = counts;
}

void sampler_fast_enable(bool on)
{
	fast_en = on;
}

void sampler_get_stats(struct sampler_stats *s)
{
	*s = stats;
}
