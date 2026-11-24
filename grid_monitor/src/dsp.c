#include <math.h>
#include <string.h>
#include "dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float hann[FFT_N];
static float wk_in[FFT_N];
static float wk_out[FFT_N];

void dsp_init(void)
{
	for (uint32_t n = 0; n < FFT_N; n++) {
		hann[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n /
					      (float)(FFT_N - 1)));
	}
	fft_port_init();
}

static inline float bin_mag(const float *X, uint32_t k)
{
	return hypotf(X[2 * k], X[2 * k + 1]);
}

/* largest magnitude among bins k-1, k, k+1 */
static uint32_t peak_bin(const float *X, uint32_t k)
{
	uint32_t best = k;
	float bm = bin_mag(X, k);

	if (k > 1 && bin_mag(X, k - 1) > bm) {
		best = k - 1;
		bm = bin_mag(X, k - 1);
	}
	if (k + 1 < FFT_N / 2 && bin_mag(X, k + 1) > bm) {
		best = k + 1;
	}
	return best;
}

/* amplitude (counts, peak) of a Hann-windowed real FFT bin */
static inline float amp(const float *X, uint32_t k)
{
	return 4.0f * bin_mag(X, k) / (float)FFT_N;
}

bool dsp_features(const uint16_t *v, const uint16_t *i, const struct app_cfg *c,
		  struct features *f)
{
	memset(f, 0, sizeof(*f));

	/* ---- time-domain stats of the current (crest / form factor) ---- */
	double sv = 0, si = 0;

	for (uint32_t n = 0; n < FFT_N; n++) {
		sv += v[n];
		si += i[n];
	}
	float mv = (float)(sv / FFT_N);
	float mi = (float)(si / FFT_N);
	double ss = 0.0, sabs = 0.0;
	float pk = 0;

	for (uint32_t n = 0; n < FFT_N; n++) {
		float d = (float)i[n] - mi;

		ss += (double)d * (double)d;
		sabs += (double)fabsf(d);
		if (fabsf(d) > pk) {
			pk = fabsf(d);
		}
	}
	float irms_c = (float)sqrt(ss / FFT_N);
	float iabs_c = (float)(sabs / (double)FFT_N);

	if (irms_c < 1.0f) {
		return false;      /* less than one count of signal */
	}
	f->crest = pk / irms_c;
	f->form = (iabs_c > 0.0f) ? irms_c / iabs_c : 0.0f;

	float sgn = c->invert_i ? -1.0f : 1.0f;

	/* ---- voltage spectrum: find the fundamental bin ---- */
	for (uint32_t n = 0; n < FFT_N; n++) {
		wk_in[n] = ((float)v[n] - mv) * hann[n];
	}
	fft_port_rfft(wk_in, wk_out);

	uint32_t kf = 10;
	float best = 0;

	for (uint32_t k = 8; k <= 12; k++) {      /* 40 .. 60 Hz */
		float m = bin_mag(wk_out, k);

		if (m > best) {
			best = m;
			kf = k;
		}
	}
	if (best < 1.0f) {
		return false;      /* no voltage fundamental */
	}
	float vre = wk_out[2 * kf], vim = wk_out[2 * kf + 1];
	float v1_counts_peak = amp(wk_out, peak_bin(wk_out, kf));

	/* ---- current spectrum ---- */
	for (uint32_t n = 0; n < FFT_N; n++) {
		wk_in[n] = sgn * ((float)i[n] - mi) * hann[n];
	}
	fft_port_rfft(wk_in, wk_out);

	float ire = wk_out[2 * kf], iim = wk_out[2 * kf + 1];
	float i1_peak = amp(wk_out, peak_bin(wk_out, kf));

	if (i1_peak < 0.5f) {
		return false;
	}
	float ki = cfg_ki(c);
	const float r2 = 0.70710678f;

	f->i1 = i1_peak * r2 * ki;
	f->vrms1 = v1_counts_peak * r2 * c->k_v;

	float hsum2 = 0.0f;
	uint32_t H = c->thd_max_h;

	if (H < 2) {
		H = 2;
	}
	if (H > 50) {
		H = 50;
	}
	for (uint32_t h = 2; h <= H; h++) {
		uint32_t k = h * kf;

		if (k + 1 >= FFT_N / 2) {
			break;
		}
		float a = amp(wk_out, peak_bin(wk_out, k));

		hsum2 += a * a;
		if (h == 3) {
			f->h3 = a / i1_peak;
		} else if (h == 5) {
			f->h5 = a / i1_peak;
		} else if (h == 7) {
			f->h7 = a / i1_peak;
		}
	}
	f->thd = sqrtf(hsum2) / i1_peak;

	/* phase: positive = current lags voltage (inductive) */
	float phi = atan2f(vim, vre) - atan2f(iim, ire);

	while (phi > (float)M_PI) {
		phi -= 2.0f * (float)M_PI;
	}
	while (phi <= -(float)M_PI) {
		phi += 2.0f * (float)M_PI;
	}
	f->phi1_deg = phi * 180.0f / (float)M_PI;
	f->cosphi1 = cosf(phi);
	f->q1 = f->vrms1 * f->i1 * sinf(phi);
	f->valid = true;
	return true;
}

enum load_class dsp_classify(const struct app_cfg *c, float irms, float pf,
			     const struct features *f, float inrush)
{
	float apf = fabsf(pf);

	if (irms < c->t_min_irms) {
		return LOAD_NONE;
	}
	if (!f || !f->valid) {
		return LOAD_UNKNOWN;
	}
	if (f->thd >= c->t_thd_smps && f->h3 >= c->t_h3_smps) {
		return LOAD_SMPS;
	}
	if (f->thd < c->t_thd_res && apf >= c->t_pf_res) {
		return LOAD_RESISTIVE;
	}
	if (f->thd < c->t_thd_smps &&
	    (apf < c->t_pf_res || inrush >= c->t_inrush_motor)) {
		return LOAD_MOTOR;
	}
	return LOAD_UNKNOWN;
}

/* ====================== transient tracker ============================= */

void tracker_init(struct tracker *t)
{
	memset(t, 0, sizeof(*t));
	t->state = TR_SETTLING;
	t->inrush = 1.0f;
	t->accepted = LOAD_UNKNOWN;
	t->cand = LOAD_UNKNOWN;
}

static void start_event(struct tracker *t, const struct cycle_metrics *m)
{
	t->state = TR_SETTLING;
	t->steady_cnt = 0;
	t->peak_since_event = m->ipeak;
	t->cand = LOAD_UNKNOWN;
	t->cand_cnt = 0;
}

void tracker_on_cycle(struct tracker *t, const struct app_cfg *c,
		      const struct cycle_metrics *m, struct tracker_out *out)
{
	memset(out, 0, sizeof(*out));
	if (!m->valid) {
		t->have_prev = false;
		t->state = TR_SETTLING;
		t->steady_cnt = 0;
		return;
	}

	float eps = c->t_eps_a;

	if (0.03f * m->irms > eps) {
		eps = 0.03f * m->irms;
	}

	if (!t->have_prev) {
		t->have_prev = true;
		start_event(t, m);
		t->prev_irms = m->irms;
		t->prev_p = m->p;
		return;
	}

	/* "no load" detection (needs several cycles in a row) */
	if (m->irms < c->t_min_irms) {
		if (++t->none_cnt >= 5 && t->accepted != LOAD_NONE) {
			t->accepted = LOAD_NONE;
			out->class_changed = true;
		}
	} else {
		t->none_cnt = 0;
	}

	float dp = m->p - t->prev_p;

	if (fabsf(dp) > c->t_event_w) {
		start_event(t, m);
		out->event = true;
	} else if (t->state == TR_SETTLING) {
		if (m->ipeak > t->peak_since_event) {
			t->peak_since_event = m->ipeak;
		}
		if (fabsf(m->irms - t->prev_irms) < eps) {
			t->steady_cnt++;
		} else {
			t->steady_cnt = 0;
		}
		if (t->steady_cnt >= c->steady_cycles) {
			t->state = TR_STEADY;
			t->steady_irms = m->irms;
			t->inrush = (m->irms < c->t_min_irms || m->ipeak < 1e-3f) ?
				    1.0f : t->peak_since_event / m->ipeak;
			t->since_fft = c->fft_interval;   /* run FFT at once */
			out->need_fft = true;
		}
	} else { /* STEADY */
		float lim = 3.0f * eps;

		if (0.15f * t->steady_irms > lim) {
			lim = 0.15f * t->steady_irms;
		}
		if (fabsf(m->irms - t->steady_irms) > lim) {
			start_event(t, m);
			out->event = true;
		} else if (++t->since_fft >= c->fft_interval) {
			t->since_fft = 0;
			out->need_fft = true;
		}
	}
	t->prev_irms = m->irms;
	t->prev_p = m->p;
}

void tracker_on_features(struct tracker *t, const struct app_cfg *c,
			 const struct cycle_metrics *m, const struct features *f,
			 struct tracker_out *out)
{
	memset(out, 0, sizeof(*out));
	if (t->state != TR_STEADY) {
		return;     /* never classify during a transient */
	}
	enum load_class cls = dsp_classify(c, m->irms, m->pf, f, t->inrush);

	if (cls == t->cand) {
		t->cand_cnt++;
	} else {
		t->cand = cls;
		t->cand_cnt = 1;
	}
	if (t->cand_cnt >= c->debounce && t->cand != t->accepted) {
		t->accepted = t->cand;
		out->class_changed = true;
	}
}
