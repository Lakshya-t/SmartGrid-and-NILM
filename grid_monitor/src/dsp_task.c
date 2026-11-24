/*
 * dsp_task.c - medium-priority thread: receives per-cycle metrics over zbus,
 * runs the transient tracker, takes a 2048-sample window when asked, computes
 * the FFT features and classifies the appliance.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "protect.h"
#include "sampler.h"
#include "dsp.h"

LOG_MODULE_REGISTER(dsp, LOG_LEVEL_INF);

ZBUS_MSG_SUBSCRIBER_DEFINE(dsp_sub);

static struct tracker trk;
static uint16_t wv[FFT_N], wi[FFT_N];

static void announce(const struct cycle_metrics *m)
{
	enum load_class cls = trk.accepted;

	protect_set_class(cls);
	LOG_INF("Load: %s  (Irms %d mA, P %d W, PF x100 %d)", load_class_str(cls),
		(int)(m->irms * 1000.0f), (int)m->p, (int)(m->pf * 100.0f));
	if ((g_cfg.banned_mask >> cls) & 1u) {
		LOG_WRN("Policy: appliance class not allowed -> trip");
		(void)protect_trip(TRIP_POLICY, (float)cls);
	}
}

static void dsp_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	const struct zbus_channel *chan;
	struct cycle_metrics m;

	k_thread_name_set(NULL, "dsp_fft");
	dsp_init();
	tracker_init(&trk);

	for (;;) {
		if (zbus_sub_wait_msg(&dsp_sub, &chan, &m, K_FOREVER) != 0) {
			continue;
		}
		struct tracker_out o;

		tracker_on_cycle(&trk, &g_cfg, &m, &o);
		if (o.class_changed) {
			announce(&m);
		}
		if (o.need_fft && sampler_copy_window(wv, wi, FFT_N)) {
			struct features f;

			if (dsp_features(wv, wi, &g_cfg, &f)) {
				f.t_ms = k_uptime_get_32();
				protect_set_features(&f);
				protect_count_feature_run();
				tracker_on_features(&trk, &g_cfg, &m, &f, &o);
				if (o.class_changed) {
					announce(&m);
				}
			}
		}
	}
}

K_THREAD_DEFINE(dsp_tid, 4096, dsp_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(2), 0, 0);
