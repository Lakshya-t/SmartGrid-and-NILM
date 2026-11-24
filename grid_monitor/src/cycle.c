#include <math.h>
#include <string.h>
#include "cycle.h"

static void reset_cur(struct cycle_det *d)
{
	memset(&d->cur, 0, sizeof(d->cur));
	d->cur.imax = -1;
	d->cur.imin = 0x7fffffff;
}

void cycle_init(struct cycle_det *d)
{
	memset(d, 0, sizeof(*d));
	d->ofs_v = 2048.0f;
	d->ofs_i = 2048.0f;
	reset_cur(d);
}

static void accumulate(struct cycle_raw *c, uint16_t v, uint16_t i)
{
	c->n++;
	c->sv += v;
	c->si += i;
	c->svv += (int64_t)v * v;
	c->sii += (int64_t)i * i;
	c->svi += (int64_t)v * i;
	if ((int32_t)i > c->imax) {
		c->imax = i;
	}
	if ((int32_t)i < c->imin) {
		c->imin = i;
	}
}

enum cyc_ev cycle_push(struct cycle_det *d, uint16_t v, uint16_t i,
		       struct cycle_raw *out)
{
	enum cyc_ev ev = CYC_NONE;

	/* Phase 1: measure the bias (about 2048 counts) over ~10 cycles. */
	if (!d->warm_done) {
		d->warm_sv += v;
		d->warm_si += i;
		if (++d->warm_n >= CYC_WARM_SAMPLES) {
			d->ofs_v = (float)d->warm_sv / (float)d->warm_n;
			d->ofs_i = (float)d->warm_si / (float)d->warm_n;
			d->warm_done = true;
		}
		return CYC_NONE;
	}

	float x = (float)v - d->ofs_v;

	d->since_cross++;

	if (x <= -(float)CYC_HYST) {
		d->armed = true;
	} else if (d->armed && x >= 0.0f) {
		/* rising zero crossing: this sample starts a new cycle */
		d->armed = false;
		if (d->in_cycle && d->cur.n >= CYC_N_MIN && d->cur.n <= CYC_N_MAX) {
			*out = d->cur;
			ev = CYC_DONE;
			/* the mean over one full cycle IS the bias; smooth it */
			float mv = (float)((double)d->cur.sv / d->cur.n);
			float mi = (float)((double)d->cur.si / d->cur.n);

			d->ofs_v += 0.25f * (mv - d->ofs_v);
			d->ofs_i += 0.25f * (mi - d->ofs_i);
		}
		d->since_cross = 1;
		reset_cur(d);
		d->in_cycle = true;
	} else if (d->since_cross > CYC_NOSIG_SAMPLES) {
		d->since_cross = 0;
		d->armed = false;
		d->in_cycle = false;
		reset_cur(d);
		ev = CYC_NOSIG;
	}

	if (d->in_cycle) {
		accumulate(&d->cur, v, i);
	}
	return ev;
}

void cycle_finish(const struct cycle_raw *r, const struct app_cfg *c,
		  float fs, struct cycle_metrics *m)
{
	double n  = (double)r->n;
	double mv = (double)r->sv / n;
	double mi = (double)r->si / n;
	double vv = (double)r->svv / n - mv * mv;
	double ii = (double)r->sii / n - mi * mi;
	double vi = (double)r->svi / n - mv * mi;

	if (vv < 0.0) {
		vv = 0.0;
	}
	if (ii < 0.0) {
		ii = 0.0;
	}

	double kv = c->k_v;
	double ki = cfg_ki(c);
	double vrms = sqrt(vv) * kv;
	double irms = sqrt(ii) * ki;
	double p = vi * kv * ki * (c->invert_i ? -1.0 : 1.0);
	double s = vrms * irms;
	double pf = (s > 1e-9) ? p / s : 0.0;
	double q2 = s * s - p * p;
	double dpk = (double)r->imax - mi;

	if (mi - (double)r->imin > dpk) {
		dpk = mi - (double)r->imin;
	}
	if (pf > 1.0) {
		pf = 1.0;
	}
	if (pf < -1.0) {
		pf = -1.0;
	}

	m->valid     = true;
	m->n         = (uint16_t)r->n;
	m->freq_hz   = fs / (float)r->n;
	m->vrms      = (float)vrms;
	m->irms      = (float)irms;
	m->p         = (float)p;
	m->s         = (float)s;
	m->q         = (float)sqrt(q2 > 0.0 ? q2 : 0.0);
	m->pf        = (float)pf;
	m->ipeak     = (float)(dpk * ki);
	m->v_cnt_rms = (float)sqrt(vv);
	m->i_cnt_rms = (float)sqrt(ii);
	m->ofs_v     = (float)mv;
	m->ofs_i     = (float)mi;
}
