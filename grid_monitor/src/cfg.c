#include <string.h>
#include "app.h"

struct app_cfg g_cfg;

static const char *const class_names[LOAD_CLASS_COUNT] = {
	"Unknown", "No load", "Resistive", "Switch-mode supply", "Motor/inductive"
};
static const char *const reason_names[TRIP_REASON_COUNT] = {
	"none", "peak current", "rms current", "thermal overload",
	"policy (banned appliance)", "manual", "sensor/sampling lost", "voltage"
};

const char *load_class_str(enum load_class c)
{
	return (c < LOAD_CLASS_COUNT) ? class_names[c] : "?";
}

const char *trip_reason_str(enum trip_reason r)
{
	return (r < TRIP_REASON_COUNT) ? reason_names[r] : "?";
}

void cfg_defaults(struct app_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->magic         = CFG_MAGIC;
	c->version       = CFG_VERSION;
	/* PLACEHOLDER: run "grid cal v <volts>" against a multimeter. */
	c->k_v           = 0.33f;
	/* (3.3 V / 4096) / (0.110 V per A) for a 220 ohm burden, 1 turn */
	c->k_i1          = 0.007324f;
	c->turns         = 1;
	c->invert_i      = 0;
	c->auto_on       = 1;
	c->i_peak_limit  = 14.0f;
	c->i_rms_limit   = 8.0f;
	c->rms_cycles    = 3;
	c->i_rated       = 6.0f;
	c->e_max         = 60.0f;
	c->v_high        = 0.0f;
	c->v_low         = 0.0f;
	c->banned_mask   = 0;
	c->t_min_irms    = 0.015f;
	c->t_thd_res     = 0.20f;
	c->t_pf_res      = 0.90f;
	c->t_thd_smps    = 0.20f;
	c->t_h3_smps     = 0.10f;
	c->t_inrush_motor = 1.6f;
	c->t_event_w     = 6.0f;
	c->t_eps_a       = 0.015f;
	c->steady_cycles = 12;
	c->fft_interval  = 10;
	c->debounce      = 3;
	c->thd_max_h     = 50;
}

float cfg_ki(const struct app_cfg *c)
{
	uint32_t t = c->turns ? c->turns : 1;

	return c->k_i1 / (float)t;
}
