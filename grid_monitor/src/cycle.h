/*
 * cycle.h - mains-cycle detector and per-cycle power maths.
 * Pure C: no Zephyr or hardware dependencies, so it is unit-tested on a PC.
 */
#ifndef CYCLE_H_
#define CYCLE_H_

#include <stdint.h>
#include <stdbool.h>
#include "app.h"

#define CYC_WARM_SAMPLES   2048   /* initial offset measurement           */
#define CYC_HYST           10     /* counts below zero needed to re-arm   */
#define CYC_NOSIG_SAMPLES  600    /* ~58 ms without a crossing = no mains */
#define CYC_N_MIN          150    /* 68 Hz                                */
#define CYC_N_MAX          260    /* 39 Hz                                */

struct cycle_raw {
	uint32_t n;
	int64_t  sv, si;       /* sums of raw counts                          */
	int64_t  svv, sii, svi;
	int32_t  imax, imin;
};

struct cycle_det {
	bool     warm_done;
	uint32_t warm_n;
	int64_t  warm_sv, warm_si;
	float    ofs_v, ofs_i;
	bool     armed, in_cycle;
	uint32_t since_cross;
	struct cycle_raw cur;
};

enum cyc_ev { CYC_NONE = 0, CYC_DONE, CYC_NOSIG };

void cycle_init(struct cycle_det *d);

/* Feed one simultaneous (voltage, current) sample pair of raw ADC counts. */
enum cyc_ev cycle_push(struct cycle_det *d, uint16_t v, uint16_t i,
		       struct cycle_raw *out);

/* Turn raw sums into real units. fs = actual sampling rate in Hz. */
void cycle_finish(const struct cycle_raw *r, const struct app_cfg *c,
		  float fs, struct cycle_metrics *m);

#endif
