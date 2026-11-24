/*
 * grid_shell.c - the "grid" command family on the UART shell.
 *   grid help
 */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "app.h"
#include "protect.h"
#include "sampler.h"
#include "logstore.h"

/* ------------------------------------------------------------ status -- */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct cycle_metrics m;
	struct features f;
	enum load_class cls;

	protect_get_snapshot(&m, &f, &cls);
	if (protect_is_tripped()) {
		shell_print(sh, "relay      : OFF - TRIPPED (%s). Use 'grid reset' after fixing the cause.",
			    trip_reason_str(protect_last_reason()));
	} else {
		shell_print(sh, "relay      : %s", protect_relay_state() ? "ON (load energised)" : "OFF");
	}
	if (!m.valid) {
		shell_print(sh, "mains      : no usable voltage signal yet");
	} else {
		shell_print(sh, "mains      : %.1f V rms, %.2f Hz", (double)m.vrms, (double)m.freq_hz);
		shell_print(sh, "current    : %.3f A rms, %.3f A peak", (double)m.irms, (double)m.ipeak);
		shell_print(sh, "power      : P %.1f W   S %.1f VA   Q(non-active) %.1f var   PF %.3f",
			    (double)m.p, (double)m.s, (double)m.q, (double)m.pf);
		if (m.p < -0.2f * m.s && m.s > 5.0f) {
			shell_warn(sh, "P is negative: clamp is probably backwards (try: grid cfg set invert_i 1)");
		}
	}
	shell_print(sh, "load class : %s", load_class_str(cls));
	if (f.valid) {
		shell_print(sh, "FFT        : I1 %.3f A  h3 %.2f h5 %.2f h7 %.2f  THD %.1f %%",
			    (double)f.i1, (double)f.h3, (double)f.h5, (double)f.h7, (double)(f.thd * 100.0f));
	}
	return 0;
}

static int cmd_feat(const struct shell *sh, size_t argc, char **argv)
{
	struct features f;

	protect_get_snapshot(NULL, &f, NULL);
	if (!f.valid) {
		shell_print(sh, "no FFT result yet (needs a steady load)");
		return 0;
	}
	shell_print(sh, "I1 %.4f A   Vrms1 %.1f V", (double)f.i1, (double)f.vrms1);
	shell_print(sh, "h3 %.3f  h5 %.3f  h7 %.3f  THD %.3f", (double)f.h3, (double)f.h5,
		    (double)f.h7, (double)f.thd);
	shell_print(sh, "phi1 %.1f deg (+ = current lags)  cos(phi1) %.3f  Q1 %.1f var",
		    (double)f.phi1_deg, (double)f.cosphi1, (double)f.q1);
	shell_print(sh, "crest %.2f  form %.2f", (double)f.crest, (double)f.form);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct sampler_stats s;
	struct protect_stats p;

	sampler_get_stats(&s);
	protect_get_stats(&p);
	shell_print(sh, "timer clock %u Hz, ARR %u, sampling rate %.3f Hz", s.timer_hz, s.arr,
		    (double)sampler_fs());
	shell_print(sh, "blocks %u  adc_err %u  dma_err %u  fast_trips %u", s.blocks, s.adc_err,
		    s.dma_err, s.fast_trips);
	shell_print(sh, "cycles %u  no-signal %u  ring overruns %u  fft runs %u", p.cycles, p.nosig,
		    p.overruns, p.feature_runs);
	return 0;
}

/* -------------------------------------------------------------- dump -- */

static uint16_t dv[FFT_N], di[FFT_N];

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = 1024;

	if (argc > 1) {
		n = (uint32_t)strtoul(argv[1], NULL, 10);
	}
	if (n < 16 || n > FFT_N) {
		shell_error(sh, "n must be 16..%u", (unsigned)FFT_N);
		return -EINVAL;
	}
	if (!sampler_copy_window(dv, di, n)) {
		shell_error(sh, "not enough samples yet, try again");
		return -EAGAIN;
	}
	shell_print(sh, "DUMP BEGIN fs=%.3f n=%u kv=%.6f ki=%.6f", (double)sampler_fs(),
		    (unsigned)n, (double)g_cfg.k_v, (double)cfg_ki(&g_cfg));
	for (uint32_t k = 0; k < n; k++) {
		shell_print(sh, "%u,%u", dv[k], di[k]);
	}
	shell_print(sh, "DUMP END");
	return 0;
}

/* --------------------------------------------------------- fault log -- */

static int cmd_fault_log(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "clear") == 0) {
		int rc = logstore_clear_faults();

		shell_print(sh, rc ? "clear failed (%d)" : "fault log cleared", rc);
		return rc;
	}
	uint32_t total = logstore_fault_count();

	shell_print(sh, "fault log: %u event(s) recorded in total (newest first)", total);
	for (uint32_t k = 0; k < 32; k++) {
		struct fault_event e;

		if (logstore_read_fault(k, &e) != 0) {
			break;
		}
		shell_print(sh, "#%u  t=%u ms  %s  | load=%s  value=%.3f  Irms=%.3f A  Ipeak=%.3f A  Vrms=%.1f V  P=%.1f W  THD=%.2f",
			    e.seq, e.t_ms, trip_reason_str((enum trip_reason)e.reason),
			    load_class_str((enum load_class)e.load_class), (double)e.value,
			    (double)e.irms, (double)e.ipeak, (double)e.vrms, (double)e.p, (double)e.thd);
	}
	return 0;
}

/* ------------------------------------------------------------- relay -- */

static int cmd_on(const struct shell *sh, size_t argc, char **argv)
{
	if (!protect_relay_on()) {
		shell_error(sh, "a trip is latched: use 'grid reset' after fixing the cause");
		return -EPERM;
	}
	shell_print(sh, "relay ON");
	return 0;
}

static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{
	protect_relay_off();
	shell_print(sh, "relay OFF");
	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	protect_reset();
	shell_print(sh, "trip cleared, relay ON");
	return 0;
}

static int cmd_trip(const struct shell *sh, size_t argc, char **argv)
{
	(void)protect_trip(TRIP_MANUAL, 0.0f);
	shell_print(sh, "manual trip issued");
	return 0;
}

/* --------------------------------------------------------- calibrate -- */

static int cmd_cal(const struct shell *sh, size_t argc, char **argv)
{
	struct cycle_metrics m;

	if (argc != 3) {
		shell_error(sh, "usage: grid cal v <volts_rms> | grid cal i <amps_rms>");
		return -EINVAL;
	}
	float val = strtof(argv[2], NULL);

	protect_get_snapshot(&m, NULL, NULL);
	if (!m.valid || val <= 0.0f) {
		shell_error(sh, "need a valid mains signal and a positive value");
		return -EINVAL;
	}
	if (argv[1][0] == 'v') {
		if (m.v_cnt_rms < 5.0f) {
			shell_error(sh, "voltage signal too small");
			return -EINVAL;
		}
		g_cfg.k_v = val / m.v_cnt_rms;
		shell_print(sh, "k_v = %.6f V/count  (read the multimeter, then 'grid cfg save')",
			    (double)g_cfg.k_v);
	} else if (argv[1][0] == 'i') {
		if (m.i_cnt_rms < 2.0f) {
			shell_error(sh, "current signal too small - connect a known load");
			return -EINVAL;
		}
		g_cfg.k_i1 = val * (float)g_cfg.turns / m.i_cnt_rms;
		shell_print(sh, "k_i1 = %.6f A/count at 1 turn  (turns = %u)", (double)g_cfg.k_i1,
			    (unsigned)g_cfg.turns);
	} else {
		return -EINVAL;
	}
	return 0;
}

/* --------------------------------------------------------------- cfg -- */

enum ft { T_F, T_U };
struct field {
	const char *name;
	enum ft t;
	size_t off;
};
#define FF(n) { #n, T_F, offsetof(struct app_cfg, n) }
#define FU(n) { #n, T_U, offsetof(struct app_cfg, n) }

static const struct field fields[] = {
	FF(k_v), FF(k_i1), FU(turns), FU(invert_i), FU(auto_on),
	FF(i_peak_limit), FF(i_rms_limit), FU(rms_cycles), FF(i_rated), FF(e_max),
	FF(v_high), FF(v_low), FU(banned_mask),
	FF(t_min_irms), FF(t_thd_res), FF(t_pf_res), FF(t_thd_smps), FF(t_h3_smps),
	FF(t_inrush_motor), FF(t_event_w), FF(t_eps_a),
	FU(steady_cycles), FU(fft_interval), FU(debounce), FU(thd_max_h),
};

static void show_field(const struct shell *sh, const struct field *f)
{
	const uint8_t *base = (const uint8_t *)&g_cfg;

	if (f->t == T_F) {
		shell_print(sh, "  %-16s = %.6g", f->name, (double)*(const float *)(base + f->off));
	} else {
		shell_print(sh, "  %-16s = %u", f->name, *(const uint32_t *)(base + f->off));
	}
}

static int cmd_cfg(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: grid cfg show | set <name> <value> | save | default");
		return -EINVAL;
	}
	if (strcmp(argv[1], "show") == 0) {
		for (size_t k = 0; k < ARRAY_SIZE(fields); k++) {
			show_field(sh, &fields[k]);
		}
		return 0;
	}
	if (strcmp(argv[1], "default") == 0) {
		cfg_defaults(&g_cfg);
		shell_print(sh, "defaults loaded (not saved yet)");
		return 0;
	}
	if (strcmp(argv[1], "save") == 0) {
		int rc = logstore_save_cfg(&g_cfg);

		shell_print(sh, rc ? "save failed (%d)" : "configuration saved to flash", rc);
		return rc;
	}
	if (strcmp(argv[1], "set") == 0 && argc == 4) {
		for (size_t k = 0; k < ARRAY_SIZE(fields); k++) {
			if (strcmp(argv[2], fields[k].name) != 0) {
				continue;
			}
			uint8_t *base = (uint8_t *)&g_cfg;

			if (fields[k].t == T_F) {
				*(float *)(base + fields[k].off) = strtof(argv[3], NULL);
			} else {
				*(uint32_t *)(base + fields[k].off) = (uint32_t)strtoul(argv[3], NULL, 0);
			}
			/* keep a few values inside safe ranges */
			if (g_cfg.turns < 1) { g_cfg.turns = 1; }
			if (g_cfg.turns > 50) { g_cfg.turns = 50; }
			if (g_cfg.thd_max_h < 2) { g_cfg.thd_max_h = 2; }
			if (g_cfg.thd_max_h > 50) { g_cfg.thd_max_h = 50; }
			if (g_cfg.steady_cycles < 2) { g_cfg.steady_cycles = 2; }
			if (g_cfg.fft_interval < 1) { g_cfg.fft_interval = 1; }
			if (g_cfg.debounce < 1) { g_cfg.debounce = 1; }
			if (g_cfg.rms_cycles < 1) { g_cfg.rms_cycles = 1; }
			show_field(sh, &fields[k]);
			return 0;
		}
		shell_error(sh, "unknown field '%s' (see: grid cfg show)", argv[2]);
		return -EINVAL;
	}
	shell_error(sh, "usage: grid cfg show | set <name> <value> | save | default");
	return -EINVAL;
}

/* ------------------------------------------------------------ policy -- */

static int class_from_name(const char *s)
{
	if (strcmp(s, "resistive") == 0) { return LOAD_RESISTIVE; }
	if (strcmp(s, "smps") == 0)      { return LOAD_SMPS; }
	if (strcmp(s, "motor") == 0)     { return LOAD_MOTOR; }
	if (strcmp(s, "unknown") == 0)   { return LOAD_UNKNOWN; }
	return -1;
}

static int cmd_policy(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
		g_cfg.banned_mask = 0;
	} else if (argc == 3 && (strcmp(argv[1], "ban") == 0 || strcmp(argv[1], "allow") == 0)) {
		int c = class_from_name(argv[2]);

		if (c < 0) {
			shell_error(sh, "class must be: resistive | smps | motor | unknown");
			return -EINVAL;
		}
		if (argv[1][0] == 'b') {
			g_cfg.banned_mask |= (1u << c);
		} else {
			g_cfg.banned_mask &= ~(1u << c);
		}
	} else if (argc != 2 || strcmp(argv[1], "show") != 0) {
		shell_error(sh, "usage: grid policy show | ban <class> | allow <class> | clear");
		return -EINVAL;
	}
	shell_print(sh, "banned classes:");
	bool any = false;

	for (int c = 0; c < LOAD_CLASS_COUNT; c++) {
		if ((g_cfg.banned_mask >> c) & 1u) {
			shell_print(sh, "  %s", load_class_str((enum load_class)c));
			any = true;
		}
	}
	if (!any) {
		shell_print(sh, "  (none)");
	}
	return 0;
}

/* -------------------------------------------------------------- help -- */

static int cmd_help(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh,
		"grid status              live power numbers, relay and load class\n"
		"grid feat                FFT features (harmonics, THD, phase)\n"
		"grid stats               sampler and thread statistics\n"
		"grid dump [n]            raw samples as CSV (for tools/analyze_dump.py)\n"
		"grid fault_log [clear]   trip history stored in flash\n"
		"grid on | off | reset    relay control (reset clears a latched trip)\n"
		"grid trip                manual trip (test)\n"
		"grid cal v <V>           calibrate voltage against a multimeter\n"
		"grid cal i <A>           calibrate current against a known load\n"
		"grid cfg show|set|save|default   settings and thresholds\n"
		"grid policy show|ban|allow|clear <resistive|smps|motor|unknown>\n"
		"kernel threads           thread priorities, stack and CPU use");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_grid,
	SHELL_CMD_ARG(help, NULL, "list commands", cmd_help, 1, 0),
	SHELL_CMD_ARG(status, NULL, "live status", cmd_status, 1, 0),
	SHELL_CMD_ARG(feat, NULL, "FFT features", cmd_feat, 1, 0),
	SHELL_CMD_ARG(stats, NULL, "statistics", cmd_stats, 1, 0),
	SHELL_CMD_ARG(dump, NULL, "dump [n]: raw samples", cmd_dump, 1, 1),
	SHELL_CMD_ARG(fault_log, NULL, "fault_log [clear]", cmd_fault_log, 1, 1),
	SHELL_CMD_ARG(on, NULL, "relay on", cmd_on, 1, 0),
	SHELL_CMD_ARG(off, NULL, "relay off", cmd_off, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "clear trip, relay on", cmd_reset, 1, 0),
	SHELL_CMD_ARG(trip, NULL, "manual trip", cmd_trip, 1, 0),
	SHELL_CMD_ARG(cal, NULL, "cal v|i <value>", cmd_cal, 3, 0),
	SHELL_CMD_ARG(cfg, NULL, "cfg show|set|save|default", cmd_cfg, 2, 2),
	SHELL_CMD_ARG(policy, NULL, "policy show|ban|allow|clear", cmd_policy, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(grid, &sub_grid, "Grid monitor commands", NULL);
