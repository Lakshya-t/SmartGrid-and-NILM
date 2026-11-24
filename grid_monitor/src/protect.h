/*
 * protect.h - protection thread, relay control and the shared latest state.
 */
#ifndef PROTECT_H_
#define PROTECT_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include "app.h"

ZBUS_CHAN_DECLARE(cycle_chan);          /* one struct cycle_metrics per cycle */
ZBUS_OBS_DECLARE(dsp_sub);

extern struct k_msgq fault_q;           /* struct fault_event, to telemetry   */

struct protect_stats {
	uint32_t cycles;
	uint32_t nosig;
	uint32_t overruns;
	uint32_t feature_runs;
};

int protect_init(void);                /* relay GPIO configured, load OFF    */
void protect_start(void);               /* release the protection thread      */

bool protect_relay_on(void);            /* false if a trip is latched         */
void protect_relay_off(void);
bool protect_reset(void);               /* clear trip latch, energise relay   */

/* Trip from a THREAD: opens the relay first, then records the event. */
bool protect_trip(enum trip_reason r, float value);
/* Trip from the DMA ISR: integer-only (Zephyr forbids FPU use in ISRs).
 * The protection thread writes the log entry a moment later. */
void protect_fast_trip(int32_t peak_counts);

bool protect_is_tripped(void);
bool protect_relay_state(void);
enum trip_reason protect_last_reason(void);

void protect_set_features(const struct features *f);
void protect_set_class(enum load_class c);
void protect_get_snapshot(struct cycle_metrics *m, struct features *f,
			  enum load_class *c);
void protect_get_stats(struct protect_stats *s);
void protect_count_feature_run(void);
#endif
