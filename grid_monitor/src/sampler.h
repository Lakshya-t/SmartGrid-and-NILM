/*
 * sampler.h - dual-ADC, timer-triggered, circular-DMA sampling (STM32F446).
 */
#ifndef SAMPLER_H_
#define SAMPLER_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>
#include "app.h"

struct sampler_stats {
	uint32_t blocks;
	uint32_t adc_err;
	uint32_t dma_err;
	uint32_t fast_trips;
	uint32_t timer_hz;
	uint32_t arr;
};

/* Given by the DMA interrupt every BLOCK_SAMPLES samples. */
extern struct k_sem sampler_blk_sem;

int      sampler_init(void);      /* configure GPIO/TIM2/ADC1+ADC2/DMA2 */
int      sampler_start(void);     /* start timer, ADCs and DMA          */
float    sampler_fs(void);        /* actual sampling rate in Hz         */

uint32_t sampler_widx(void);      /* total samples written so far       */
uint32_t sampler_ring_at(uint32_t idx);  /* word: low=voltage, high=current */
bool     sampler_copy_window(uint16_t *v, uint16_t *i, uint32_t n);

void     sampler_set_ofs_i(int32_t counts);
void     sampler_set_peak_counts(int32_t counts);
void     sampler_fast_enable(bool on);
void     sampler_get_stats(struct sampler_stats *s);

#endif
