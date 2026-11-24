/*
 * protect.c - highest-priority thread: consumes sample blocks, finds mains
 * cycles, computes power per cycle, runs the protection tests, publishes the
 * per-cycle metrics on zbus and drives the relay trip pin.
 *
 * Relay pin convention (see boards/nucleo_f446re.overlay):
 *   "active" = relay energised = LOAD ON.  Inactive = tripped / load OFF.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <math.h>
#include <string.h>
#include "protect.h"
#include "sampler.h"
#include "cycle.h"
#include <errno.h>
LOG_MODULE_REGISTER(protect, LOG_LEVEL_INF);

BUILD_ASSERT(sizeof(struct cycle_metrics) <= CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE,
	     "raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE");

#define SENSOR_TIMEOUT_MS 20   /* no sample block for this long = fail-safe */

static const struct gpio_dt_spec relay =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), relay_gpios);

ZBUS_CHAN_DEFINE(cycle_chan, struct cycle_metrics, NULL, NULL,
		 ZBUS_OBSERVERS(dsp_sub), ZBUS_MSG_INIT(0));

K_MSGQ_DEFINE(fault_q, sizeof(struct fault_event), 8, 4);
K_SEM_DEFINE(start_sem, 0, 1);

static atomic_t tripped;
static atomic_t relay_state;
static atomic_t last_reason;
static atomic_t g_class = ATOMIC_INIT(LOAD_UNKNOWN);
static atomic_t clear_req;
static atomic_t pending_fast;     /* set by the DMA ISR trip path */
static atomic_t pending_counts;

static struct k_spinlock lock;
static struct cycle_metrics latest;
static struct features latest_f;
static struct protect_stats pstats;

/* ----------------------------------------------------------- relay ---- */

int protect_init(void)
{
    if (!gpio_is_ready_dt(&relay)) {
        LOG_ERR("relay GPIO not ready");
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&relay, GPIO_OUTPUT_INACTIVE);

    if (ret < 0) {
        LOG_ERR("Failed to configure relay GPIO: %d", ret);
        return ret;
    }

    atomic_set(&relay_state, 0);
    atomic_set(&tripped, 0);

    LOG_INF("Relay initialized: OFF");
    return 0;
}

bool protect_relay_on(void)
{
	if (atomic_get(&tripped)) {
		return false;
	}
	gpio_pin_set_dt(&relay, 1);
	atomic_set(&relay_state, 1);
	sampler_fast_enable(true);
	return true;
}

void protect_relay_off(void)
{
	gpio_pin_set_dt(&relay, 0);
	atomic_set(&relay_state, 0);
}

bool protect_reset(void)
{
	atomic_set(&tripped, 0);
	atomic_set(&clear_req, 1);
	return protect_relay_on();
}

/* ISR-safe, integer-only: latch the trip and open the relay. */
static bool trip_latch(enum trip_reason r)
{
	if (r != TRIP_MANUAL && !atomic_get(&relay_state)) {
		return false;           /* load already disconnected */
	}
	if (!atomic_cas(&tripped, 0, 1)) {
		return false;           /* already latched */
	}
	/* open the relay FIRST: this is the time-critical step */
	gpio_pin_set_dt(&relay, 0);
	atomic_set(&relay_state, 0);
	atomic_set(&last_reason, r);
	return true;
}

/* Thread context only (uses floats): hand the record to the telemetry thread. */
static void queue_event(enum trip_reason r, float value, uint16_t counts)
{
	struct fault_event ev = {
		.t_ms = k_uptime_get_32(),
		.reason = (uint8_t)r,
		.load_class = (uint8_t)atomic_get(&g_class),
		.peak_counts = counts,
		.value = value,
	};
	k_spinlock_key_t key = k_spin_lock(&lock);

	ev.irms = latest.irms;
	ev.ipeak = latest.ipeak;
	ev.vrms = latest.vrms;
	ev.p = latest.p;
	ev.thd = latest_f.thd;
	k_spin_unlock(&lock, key);

	(void)k_msgq_put(&fault_q, &ev, K_NO_WAIT);
}

bool protect_trip(enum trip_reason r, float value)
{
	if (!trip_latch(r)) {
		return false;
	}
	queue_event(r, value, 0);
	return true;
}

void protect_fast_trip(int32_t peak_counts)
{
	if (trip_latch(TRIP_PEAK)) {
		atomic_set(&pending_counts, peak_counts);
		atomic_set(&pending_fast, 1);
	}
}

bool protect_is_tripped(void)
{
	return atomic_get(&tripped) != 0;
}

bool protect_relay_state(void)
{
	return atomic_get(&relay_state) != 0;
}

enum trip_reason protect_last_reason(void)
{
	return (enum trip_reason)atomic_get(&last_reason);
}

/* -------------------------------------------------- shared snapshots -- */

void protect_set_features(const struct features *f)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	latest_f = *f;
	k_spin_unlock(&lock, key);
}

void protect_set_class(enum load_class c)
{
	atomic_set(&g_class, c);
}

void protect_get_snapshot(struct cycle_metrics *m, struct features *f,
			  enum load_class *c)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (m) {
		*m = latest;
	}
	if (f) {
		*f = latest_f;
	}
	k_spin_unlock(&lock, key);
	if (c) {
		*c = (enum load_class)atomic_get(&g_class);
	}
}

void protect_get_stats(struct protect_stats *s)
{
	*s = pstats;
}

void protect_count_feature_run(void)
{
	pstats.feature_runs++;
}

/* ----------------------------------------------------------- thread --- */

void protect_start(void)
{
	k_sem_give(&start_sem);
}

static void update_isr_limits(const struct cycle_det *d)
{
	float ki = cfg_ki(&g_cfg);
	int32_t pk = (ki > 0.0f) ? (int32_t)(g_cfg.i_peak_limit / ki) : 1900;

	if (pk < 8) {
		pk = 8;
	}
	if (pk > 2040) {
		pk = 2040;
	}
	sampler_set_peak_counts(pk);
	sampler_set_ofs_i((int32_t)(d->ofs_i + 0.5f));
}

static void protect_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	static struct cycle_det det;
	static uint32_t over_cnt, volt_cnt;
	static float therm;
	bool auto_done = false;

	k_thread_name_set(NULL, "protection");
	cycle_init(&det);
	k_sem_take(&start_sem, K_FOREVER);
	uint32_t ridx = sampler_widx();

	for (;;) {
		if (k_sem_take(&sampler_blk_sem, K_MSEC(SENSOR_TIMEOUT_MS)) != 0) {
			/* sampling has stopped: fail safe */
			(void)protect_trip(TRIP_SENSOR, 0.0f);
			continue;
		}
		if (atomic_cas(&pending_fast, 1, 0)) {
			int32_t pc = (int32_t)atomic_get(&pending_counts);

			queue_event(TRIP_PEAK, (float)pc * cfg_ki(&g_cfg), (uint16_t)pc);
		}
		if (atomic_cas(&clear_req, 1, 0)) {
			over_cnt = 0;
			volt_cnt = 0;
			therm = 0.0f;
		}

		uint32_t w = sampler_widx();

		if ((w - ridx) > (RING_SAMPLES - BLOCK_SAMPLES)) {
			pstats.overruns++;
			ridx = w - BLOCK_SAMPLES;
		}
		while (ridx != w) {
			uint32_t s = sampler_ring_at(ridx++);
			struct cycle_raw raw;
			struct cycle_metrics m;
			enum cyc_ev ev = cycle_push(&det, (uint16_t)(s & 0xFFFFu),
						    (uint16_t)(s >> 16), &raw);

			if (ev == CYC_NONE) {
				continue;
			}
			memset(&m, 0, sizeof(m));
			m.t_ms = k_uptime_get_32();
			if (ev == CYC_DONE) {
				cycle_finish(&raw, &g_cfg, sampler_fs(), &m);
				m.t_ms = k_uptime_get_32();
				pstats.cycles++;
			} else {
				m.valid = false;
				pstats.nosig++;
			}
			m.seq = pstats.cycles + pstats.nosig;

			k_spinlock_key_t key = k_spin_lock(&lock);

			latest = m;
			k_spin_unlock(&lock, key);
			(void)zbus_chan_pub(&cycle_chan, &m, K_NO_WAIT);
			update_isr_limits(&det);

			if (!m.valid) {
				over_cnt = 0;
				volt_cnt = 0;
				continue;
			}

			/* ---- RMS over-current ---- */
			if (m.irms > g_cfg.i_rms_limit) {
				if (++over_cnt >= g_cfg.rms_cycles) {
					(void)protect_trip(TRIP_RMS, m.irms);
				}
			} else {
				over_cnt = 0;
			}
			/* ---- instantaneous peak (backup to the ISR check) ---- */
			if (m.ipeak > g_cfg.i_peak_limit) {
				(void)protect_trip(TRIP_PEAK, m.ipeak);
			}
			/* ---- thermal-style overload ---- */
			if (g_cfg.i_rated > 0.0f) {
				float T = (float)m.n / sampler_fs();

				therm += (m.irms * m.irms -
					  g_cfg.i_rated * g_cfg.i_rated) * T;
				if (therm < 0.0f) {
					therm = 0.0f;
				}
				if (therm > g_cfg.e_max) {
					(void)protect_trip(TRIP_THERMAL, therm);
				}
			}
			/* ---- voltage window (disabled unless limits are set) ---- */
			bool bad_v = (g_cfg.v_high > 0.0f && m.vrms > g_cfg.v_high) ||
				     (g_cfg.v_low > 0.0f && m.vrms < g_cfg.v_low);

			if (bad_v) {
				if (++volt_cnt >= 3) {
					(void)protect_trip(TRIP_VOLTAGE, m.vrms);
				}
			} else {
				volt_cnt = 0;
			}
		}

		/* energise the relay once the bias has been measured */
		if (!auto_done && det.warm_done) {
			auto_done = true;
			if (g_cfg.auto_on && protect_relay_on()) {
				LOG_INF("warm-up done, load energised");
			} else {
				LOG_INF("warm-up done, relay left OFF (grid on to energise)");
			}
		}
	}
}

K_THREAD_DEFINE(protect_tid, 2560, protect_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(0), 0, 0);
