/*
 * Zephyr OS Smart Grid - Micro-PMU and NILM load monitor.
 * main() only initialises the pieces; the work happens in three threads:
 *   protection (prio 0)  protect.c
 *   dsp / FFT  (prio 2)  dsp_task.c
 *   telemetry  (prio 5)  telemetry.c   (+ the UART shell, also prio 5)
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "app.h"
#include "sampler.h"
#include "protect.h"
#include "logstore.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	cfg_defaults(&g_cfg);
	int rc = protect_init();

	if (rc < 0) {
		LOG_ERR("Relay initialization failed: %d", rc);
		return rc;
	}

	if (logstore_init() == 0) {
		if (logstore_load_cfg(&g_cfg)) {
			LOG_INF("configuration loaded from flash");
		} else {
			LOG_INF("no saved configuration, using defaults");
		}
	}
	g_cfg.auto_on = 1;

	rc = sampler_init();

	if (rc) {
		LOG_ERR("sampler init failed (%d) - relay stays OFF", rc);
		return rc;
	}
	rc = sampler_start();
	if (rc) {
		LOG_ERR("sampler start failed (%d) - relay stays OFF", rc);
		return rc;
	}
	protect_start();
	LOG_INF("Grid monitor running. Type: grid help");
	return 0;
}
