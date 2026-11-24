/*
 * app.h - shared types and constants for the Grid Monitor (Micro-PMU + NILM)
 */
#ifndef APP_H_
#define APP_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- sampling geometry ------------------------------------------------ */
#define BLOCK_SAMPLES   16u      /* samples per DMA half-buffer (1.5625 ms)   */
#define RING_SAMPLES    4096u    /* raw sample ring (must be a power of two)  */
#define FFT_N           2048u    /* FFT window: 10 cycles of 50 Hz @ 10.24 kHz*/
#define FS_NOMINAL_HZ   10240.0f

/* ---- appliance classes ------------------------------------------------- */
enum load_class {
	LOAD_UNKNOWN = 0,
	LOAD_NONE,
	LOAD_RESISTIVE,
	LOAD_SMPS,
	LOAD_MOTOR,
	LOAD_CLASS_COUNT
};

/* ---- trip reasons ------------------------------------------------------ */
enum trip_reason {
	TRIP_NONE = 0,
	TRIP_PEAK,      /* instantaneous over-current (or ADC clipping)  */
	TRIP_RMS,       /* RMS over-current for several cycles           */
	TRIP_THERMAL,   /* long mild overload (I^2 t style)              */
	TRIP_POLICY,    /* appliance class not allowed                   */
	TRIP_MANUAL,    /* operator command                              */
	TRIP_SENSOR,    /* sampling stopped (fail-safe)                  */
	TRIP_VOLTAGE,   /* over/under voltage                            */
	TRIP_REASON_COUNT
};

const char *load_class_str(enum load_class c);
const char *trip_reason_str(enum trip_reason r);

/* ---- one measurement per mains cycle ------------------------------------ */
struct cycle_metrics {
	uint32_t seq;
	uint32_t t_ms;
	bool     valid;      /* false = no usable mains signal                */
	uint16_t n;          /* samples in this cycle                         */
	float    freq_hz;
	float    vrms, irms; /* real units (V, A)                             */
	float    p, s, q;    /* W, VA, var (q = sqrt(S^2-P^2), non-active)    */
	float    pf;         /* signed: P / S                                 */
	float    ipeak;      /* A                                             */
	float    v_cnt_rms;  /* rms in ADC counts (for calibration)           */
	float    i_cnt_rms;
	float    ofs_v, ofs_i;
};

/* ---- features from one FFT window ------------------------------------- */
struct features {
	bool     valid;
	uint32_t t_ms;
	float    i1;         /* fundamental current, A rms                    */
	float    h3, h5, h7; /* harmonic ratios I_n / I_1                     */
	float    thd;        /* ratio (1.0 = 100 %)                           */
	float    phi1_deg;   /* + = current lags voltage                      */
	float    cosphi1;
	float    q1;         /* fundamental reactive power, var               */
	float    vrms1;      /* fundamental voltage, V rms                    */
	float    crest, form;
};

/* ---- fault log record (stored in flash) ------------------------------- */
struct fault_event {
	uint32_t t_ms;
	uint32_t seq;
	uint8_t  reason;
	uint8_t  load_class;
	uint16_t peak_counts;   /* ADC counts, set by the fast ISR trip path */
	float    value;      /* the number that caused the trip               */
	float    irms, ipeak, vrms, p, thd;
};

/* ---- persistent configuration ----------------------------------------- */
#define CFG_MAGIC    0x47524944u   /* 'GRID' */
#define CFG_VERSION  2u

struct app_cfg {
	uint32_t magic;
	uint32_t version;
	/* calibration */
	float    k_v;           /* volts per ADC count                         */
	float    k_i1;          /* load amps per ADC count with ONE turn       */
	uint32_t turns;         /* wire turns through the clamp                */
	uint32_t invert_i;      /* 1 = clamp fitted backwards                  */
	uint32_t auto_on;       /* 1 = energise relay after warm-up            */
	/* protection */
	float    i_peak_limit;  /* A, instantaneous                            */
	float    i_rms_limit;   /* A rms                                       */
	uint32_t rms_cycles;    /* cycles above limit before trip              */
	float    i_rated;       /* A rms, thermal model                        */
	float    e_max;         /* A^2*s, thermal trip level                   */
	float    v_high;        /* V rms, 0 = disabled                         */
	float    v_low;         /* V rms, 0 = disabled                         */
	uint32_t banned_mask;   /* bit (1<<class) set = class not allowed      */
	/* classifier (ILLUSTRATIVE - tune from real measurements) */
	float    t_min_irms;    /* below this = no load                        */
	float    t_thd_res;     /* resistive: THD below                        */
	float    t_pf_res;      /* resistive: |PF| at least                    */
	float    t_thd_smps;    /* SMPS: THD at least                          */
	float    t_h3_smps;     /* SMPS: h3 at least                           */
	float    t_inrush_motor;/* motor: inrush ratio at least                */
	float    t_event_w;     /* step in P that counts as an event           */
	float    t_eps_a;       /* steady-state tolerance on Irms              */
	uint32_t steady_cycles; /* cycles of steady Irms before FFT            */
	uint32_t fft_interval;  /* cycles between FFT runs while steady        */
	uint32_t debounce;      /* identical results needed to accept a class  */
	uint32_t thd_max_h;     /* highest harmonic in THD (<= 50)             */
	uint32_t crc;           /* must stay last                              */
};

extern struct app_cfg g_cfg;

void  cfg_defaults(struct app_cfg *c);
float cfg_ki(const struct app_cfg *c);   /* load amps per ADC count */

#endif /* APP_H_ */
