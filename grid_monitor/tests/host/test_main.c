/* Host tests for cycle.c and dsp.c using synthetic mains waveforms. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cycle.h"
#include "dsp.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; \
	printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FS 10240.0

enum kind { K_NONE, K_RES, K_IND, K_SMPS, K_NOV };

static double vamp = 800.0;       /* counts peak */

static uint16_t clip(double x)
{
	if (x < 0) { x = 0; }
	if (x > 4095) { x = 4095; }
	return (uint16_t)lround(x);
}

/* amplitude of load current in counts (peak) for given rms amps, 1 turn */
static double amps_to_counts_pk(double irms)
{
	return irms * sqrt(2.0) / 0.007324;
}

struct scn {
	enum kind k;
	double f;       /* grid frequency */
	double irms;    /* amps rms (fundamental for RES/IND; scale for SMPS) */
	double phi_deg; /* IND lag */
	double noise;   /* counts rms-ish */
	int reverse;    /* clamp reversed */
};

static double frand(void) { return (double)rand() / RAND_MAX * 2.0 - 1.0; }

static void gen(const struct scn *s, double t, uint16_t *v, uint16_t *i)
{
	double th = 2.0 * M_PI * s->f * t;
	double vs = sin(th);
	double ia = 0.0;

	switch (s->k) {
	case K_RES:  ia = amps_to_counts_pk(s->irms) * vs; break;
	case K_IND:  ia = amps_to_counts_pk(s->irms) * sin(th - s->phi_deg * M_PI / 180.0); break;
	case K_SMPS: {
		/* capacitor-input rectifier: conducts only near the voltage peaks */
		double thr = 0.80, a = fabs(vs) - thr;

		ia = (a > 0 ? a / (1.0 - thr) : 0.0) * (vs > 0 ? 1 : -1) *
		     amps_to_counts_pk(s->irms);
		break;
	}
	default: break;
	}
	if (s->reverse) { ia = -ia; }
	*v = clip(2100.0 + (s->k == K_NOV ? 0.0 : vamp * vs) + s->noise * frand());
	*i = clip(2048.0 + ia + s->noise * frand());
}

struct sim {
	struct app_cfg cfg;
	struct cycle_det det;
	struct tracker tr;
	uint16_t wv[FFT_N], wi[FFT_N];
	uint32_t wpos, nsamp;
	struct cycle_metrics last;
	struct features feat;
	int cycles, nosig, ffts, class_changes;
	enum load_class last_cls;
	double t;
};

static void sim_init(struct sim *m)
{
	memset(m, 0, sizeof(*m));
	cfg_defaults(&m->cfg);
	m->cfg.k_v = (float)(230.0 * sqrt(2.0) / vamp);
	m->cfg.t_event_w = 2.0f;
	cycle_init(&m->det);
	tracker_init(&m->tr);
}

static void sim_run(struct sim *m, const struct scn *s, double seconds)
{
	uint32_t n = (uint32_t)(seconds * FS);

	for (uint32_t k = 0; k < n; k++) {
		uint16_t v, i;
		struct cycle_raw raw;

		gen(s, m->t, &v, &i);
		m->t += 1.0 / FS;
		m->wv[m->wpos] = v; m->wi[m->wpos] = i;
		m->wpos = (m->wpos + 1) % FFT_N;
		m->nsamp++;

		enum cyc_ev ev = cycle_push(&m->det, v, i, &raw);

		if (ev == CYC_NOSIG) {
			struct cycle_metrics inv = { .valid = false };
			struct tracker_out o;

			m->nosig++;
			tracker_on_cycle(&m->tr, &m->cfg, &inv, &o);
		} else if (ev == CYC_DONE) {
			struct tracker_out o;

			cycle_finish(&raw, &m->cfg, (float)FS, &m->last);
			m->cycles++;
			tracker_on_cycle(&m->tr, &m->cfg, &m->last, &o);
			if (o.class_changed) { m->class_changes++; }
			if (o.need_fft && m->nsamp >= FFT_N) {
				uint16_t v2[FFT_N], i2[FFT_N];

				for (uint32_t q = 0; q < FFT_N; q++) {   /* oldest first */
					v2[q] = m->wv[(m->wpos + q) % FFT_N];
					i2[q] = m->wi[(m->wpos + q) % FFT_N];
				}
				if (dsp_features(v2, i2, &m->cfg, &m->feat)) {
					m->ffts++;
					tracker_on_features(&m->tr, &m->cfg, &m->last, &m->feat, &o);
					if (o.class_changed) { m->class_changes++; }
				}
			}
		}
	}
}

static void test_resistive(double f)
{
	struct sim m; struct scn s = { K_RES, f, 0.5, 0, 1.0, 0 };

	sim_init(&m);
	sim_run(&m, &s, 3.0);
	printf("resistive %.1f Hz: Vrms %.1f Irms %.3f P %.1f PF %.3f f %.2f THD %.3f cos %.3f class=%s\n",
	       f, m.last.vrms, m.last.irms, m.last.p, m.last.pf, m.last.freq_hz,
	       m.feat.thd, m.feat.cosphi1, load_class_str(m.tr.accepted));
	CHECK(fabs(m.last.vrms - 230.0) < 3.0, "vrms %.2f", m.last.vrms);
	CHECK(fabs(m.last.irms - 0.5) < 0.01, "irms %.3f", m.last.irms);
	CHECK(fabs(m.last.p - 115.0) < 3.0, "p %.2f", m.last.p);
	CHECK(m.last.pf > 0.98, "pf %.3f", m.last.pf);
	CHECK(fabs(m.last.freq_hz - f) < 0.6, "freq %.2f", m.last.freq_hz);
	CHECK(m.feat.valid && m.feat.thd < 0.06, "thd %.3f", m.feat.thd);
	CHECK(m.tr.accepted == LOAD_RESISTIVE, "class %s", load_class_str(m.tr.accepted));
}

static void test_inductive(void)
{
	struct sim m; struct scn s = { K_IND, 50.0, 0.5, 45.0, 1.0, 0 };

	sim_init(&m);
	sim_run(&m, &s, 3.0);
	printf("inductive 45deg: P %.1f PF %.3f phi1 %.1f deg Q1 %.1f THD %.3f class=%s\n",
	       m.last.p, m.last.pf, m.feat.phi1_deg, m.feat.q1, m.feat.thd,
	       load_class_str(m.tr.accepted));
	CHECK(fabs(m.last.pf - 0.7071) < 0.03, "pf %.3f", m.last.pf);
	CHECK(fabs(m.feat.phi1_deg - 45.0) < 3.0, "phi1 %.1f (positive = lagging)", m.feat.phi1_deg);
	CHECK(m.feat.q1 > 0, "q1 sign %.1f", m.feat.q1);
	CHECK(fabs(m.feat.q1 - 230.0 * 0.5 * 0.7071) < 6.0, "q1 %.1f", m.feat.q1);
	CHECK(m.tr.accepted == LOAD_MOTOR, "class %s", load_class_str(m.tr.accepted));
}

static void test_smps(void)
{
	struct sim m; struct scn s = { K_SMPS, 50.0, 0.8, 0, 1.0, 0 };

	sim_init(&m);
	sim_run(&m, &s, 3.0);
	printf("smps: Irms %.3f P %.1f PF %.3f h3 %.2f h5 %.2f h7 %.2f THD %.2f crest %.2f class=%s\n",
	       m.last.irms, m.last.p, m.last.pf, m.feat.h3, m.feat.h5, m.feat.h7,
	       m.feat.thd, m.feat.crest, load_class_str(m.tr.accepted));
	CHECK(m.feat.thd > 0.5, "thd %.2f", m.feat.thd);
	CHECK(m.feat.h3 > 0.3 && m.feat.h5 > 0.1, "h3 %.2f h5 %.2f", m.feat.h3, m.feat.h5);
	CHECK(m.last.pf < 0.85 && m.last.pf > 0.3, "pf %.2f", m.last.pf);
	CHECK(m.feat.crest > 1.9, "crest %.2f", m.feat.crest);
	CHECK(m.tr.accepted == LOAD_SMPS, "class %s", load_class_str(m.tr.accepted));
	/* identity from the guide: PF = cos(phi1)/sqrt(1+THD^2) */
	double pf2 = m.feat.cosphi1 / sqrt(1.0 + m.feat.thd * m.feat.thd);

	CHECK(fabs(pf2 - m.last.pf) < 0.06, "PF identity %.3f vs %.3f", pf2, m.last.pf);
}

static void test_reversed_clamp(void)
{
	struct sim m; struct scn s = { K_RES, 50.0, 0.5, 0, 1.0, 1 };

	sim_init(&m);
	sim_run(&m, &s, 2.0);
	CHECK(m.last.p < -100.0, "reversed clamp should read negative P, got %.1f", m.last.p);
	sim_init(&m);
	m.cfg.invert_i = 1;
	sim_run(&m, &s, 2.0);
	CHECK(m.last.p > 100.0, "invert_i should fix sign, got %.1f", m.last.p);
	CHECK(m.tr.accepted == LOAD_RESISTIVE, "class %s", load_class_str(m.tr.accepted));
}

static void test_no_mains(void)
{
	struct sim m; struct scn s = { K_NOV, 50.0, 0.0, 0, 0.5, 0 };

	sim_init(&m);
	sim_run(&m, &s, 1.0);
	CHECK(m.cycles == 0, "no cycles expected, got %d", m.cycles);
	CHECK(m.nosig > 5, "nosig events %d", m.nosig);
}

static void test_transitions(void)
{
	struct sim m; struct scn none = { K_RES, 50.0, 0.0, 0, 0.7, 0 };
	struct scn smps = { K_SMPS, 50.0, 0.6, 0, 0.7, 0 };
	struct scn res = { K_RES, 50.0, 1.0, 0, 0.7, 0 };

	sim_init(&m);
	sim_run(&m, &none, 2.0);
	printf("t=2s no load -> class=%s\n", load_class_str(m.tr.accepted));
	CHECK(m.tr.accepted == LOAD_NONE || m.tr.accepted == LOAD_UNKNOWN, "idle class %s", load_class_str(m.tr.accepted));
	sim_run(&m, &smps, 2.5);
	printf("charger plugged: class=%s THD %.2f\n", load_class_str(m.tr.accepted), m.feat.thd);
	CHECK(m.tr.accepted == LOAD_SMPS, "after charger: %s", load_class_str(m.tr.accepted));
	sim_run(&m, &none, 1.5);
	printf("removed: class=%s\n", load_class_str(m.tr.accepted));
	CHECK(m.tr.accepted == LOAD_NONE, "after removal: %s", load_class_str(m.tr.accepted));
	sim_run(&m, &res, 2.5);
	printf("heater plugged: class=%s\n", load_class_str(m.tr.accepted));
	CHECK(m.tr.accepted == LOAD_RESISTIVE, "after heater: %s", load_class_str(m.tr.accepted));
}

/* Hann/FFT scaling check: a pure 50 Hz current of known size */
static void test_fft_scaling(void)
{
	struct app_cfg c; struct features f;
	static uint16_t v[FFT_N], i[FFT_N];
	double irms = 0.4;

	cfg_defaults(&c);
	c.k_v = (float)(230.0 * sqrt(2.0) / vamp);
	for (uint32_t n = 0; n < FFT_N; n++) {
		double th = 2 * M_PI * 50.0 * n / FS;

		v[n] = clip(2100 + vamp * sin(th));
		i[n] = clip(2048 + amps_to_counts_pk(irms) * sin(th));
	}
	dsp_init();
	CHECK(dsp_features(v, i, &c, &f), "features failed");
	printf("fft scaling: I1 %.4f A (want 0.400) Vrms1 %.1f V\n", f.i1, f.vrms1);
	CHECK(fabs(f.i1 - irms) < 0.01, "i1 %.4f", f.i1);
	CHECK(fabs(f.vrms1 - 230.0) < 4.0, "vrms1 %.1f", f.vrms1);
	CHECK(fabs(f.phi1_deg) < 1.0, "phi %.2f", f.phi1_deg);
}

int main(void)
{
	srand(1);
	dsp_init();
	test_fft_scaling();
	test_resistive(50.0);
	test_resistive(49.8);
	test_resistive(50.2);
	test_inductive();
	test_smps();
	test_reversed_clamp();
	test_no_mains();
	test_transitions();
	printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
