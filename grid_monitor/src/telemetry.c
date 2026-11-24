/*
 * telemetry.c - lowest-priority worker: writes fault events to flash and
 * drives the LD2 indicator (slow blink = running, solid = tripped).
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "protect.h"
#include "logstore.h"

LOG_MODULE_REGISTER(telemetry, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void telemetry_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	uint32_t tick = 0;

	k_thread_name_set(NULL, "telemetry");
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	for (;;) {
		struct fault_event ev;

		if (k_msgq_get(&fault_q, &ev, K_MSEC(250)) == 0) {
			int rc = logstore_append_fault(&ev);

			LOG_WRN("TRIP: %s (value x1000 = %d)%s",
				trip_reason_str((enum trip_reason)ev.reason),
				(int)(ev.value * 1000.0f), rc ? " [log write failed]" : "");
		}
		tick++;
		if (gpio_is_ready_dt(&led)) {
			if (protect_is_tripped()) {
				gpio_pin_set_dt(&led, 1);
			} else {
				gpio_pin_set_dt(&led, (tick & 2u) ? 1 : 0);
			}
		}
	}
}

K_THREAD_DEFINE(telemetry_tid, 3072, telemetry_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(5), 0, 0);
