/*
 * dsp.h - FFT features, appliance classifier and transient tracker.
 * Pure C (the FFT itself is behind fft_port.h) so it is unit-tested on a PC.
 */
#ifndef DSP_H_
#define DSP_H_

#include <stdint.h>
#include <stdbool.h>
#include "app.h"

/* The FFT back-end. Real FFT of FFT_N floats.
 * `in` may be destroyed. `out` is CMSIS "packed" format:
 *   out[0] = Re X[0], out[1] = Re X[N/2], then out[2k], out[2k+1] = Re, Im X[k]
 */
void fft_port_init(void);
void fft_port_rfft(float *in, float *out);

void dsp_init(void);

/* Compute features from FFT_N simultaneous raw samples. fs in Hz. */
bool dsp_features(const uint16_t *v, const uint16_t *i, const struct app_cfg *c,
		  struct features *f);

/* Pure classification rule. inrush = start-up peak / steady peak. */
enum load_class dsp_classify(const struct app_cfg *c, float irms, float pf,
			     const struct features *f, float inrush);

/* ---- transient tracker ------------------------------------------------ */
enum tr_state { TR_SETTLING = 0, TR_STEADY };

struct tracker {
	enum tr_state state;
	bool     have_prev;
	float    prev_irms, prev_p;
	float    steady_irms;
	uint32_t steady_cnt;
	uint32_t since_fft;
	uint32_t none_cnt;
	float    peak_since_event;
	float    inrush;
	enum load_class cand;
	uint32_t cand_cnt;
	enum load_class accepted;
};

struct tracker_out {
	bool need_fft;       /* run an FFT on the latest window now           */
	bool event;          /* a step change was detected                    */
	bool class_changed;  /* tracker->accepted changed                     */
};

void tracker_init(struct tracker *t);
void tracker_on_cycle(struct tracker *t, const struct app_cfg *c,
		      const struct cycle_metrics *m, struct tracker_out *out);
void tracker_on_features(struct tracker *t, const struct app_cfg *c,
			 const struct cycle_metrics *m, const struct features *f,
			 struct tracker_out *out);

#endif
