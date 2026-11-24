/*
 * logstore.c - configuration and fault log in internal flash using NVS.
 * Storage partition is defined in boards/nucleo_f446re.overlay.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <stddef.h>
#include <string.h>
#include "logstore.h"

LOG_MODULE_REGISTER(logstore, LOG_LEVEL_INF);

#define ID_CFG     1
#define ID_COUNT   2
#define ID_FAULT0  16
#define FAULT_MAX  32

static struct nvs_fs fs;
static bool ready;
K_MUTEX_DEFINE(ls_lock);

int logstore_init(void)
{
	struct flash_pages_info info;

	fs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(fs.flash_device)) {
		LOG_ERR("flash not ready");
		return -ENODEV;
	}
	fs.offset = FIXED_PARTITION_OFFSET(storage_partition);
	int rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);

	if (rc) {
		LOG_ERR("page info %d", rc);
		return rc;
	}
	fs.sector_size = info.size;
	fs.sector_count = FIXED_PARTITION_SIZE(storage_partition) / info.size;

	rc = nvs_mount(&fs);
	if (rc) {
		LOG_WRN("nvs_mount failed (%d), formatting", rc);
		(void)nvs_clear(&fs);
		rc = nvs_mount(&fs);
	}
	if (rc) {
		LOG_ERR("nvs_mount failed (%d)", rc);
		return rc;
	}
	ready = true;
	LOG_INF("flash log ready (%u sectors of %u bytes)", (unsigned)fs.sector_count,
		(unsigned)fs.sector_size);
	return 0;
}

bool logstore_load_cfg(struct app_cfg *c)
{
	struct app_cfg t;

	if (!ready) {
		return false;
	}
	k_mutex_lock(&ls_lock, K_FOREVER);
	ssize_t n = nvs_read(&fs, ID_CFG, &t, sizeof(t));

	k_mutex_unlock(&ls_lock);
	if (n != (ssize_t)sizeof(t) || t.magic != CFG_MAGIC || t.version != CFG_VERSION ||
	    t.crc != crc32_ieee((const uint8_t *)&t, offsetof(struct app_cfg, crc))) {
		return false;
	}
	*c = t;
	return true;
}

int logstore_save_cfg(const struct app_cfg *c)
{
	struct app_cfg t = *c;

	if (!ready) {
		return -ENODEV;
	}
	t.magic = CFG_MAGIC;
	t.version = CFG_VERSION;
	t.crc = crc32_ieee((const uint8_t *)&t, offsetof(struct app_cfg, crc));
	k_mutex_lock(&ls_lock, K_FOREVER);
	ssize_t n = nvs_write(&fs, ID_CFG, &t, sizeof(t));

	k_mutex_unlock(&ls_lock);
	return (n < 0) ? (int)n : 0;
}

uint32_t logstore_fault_count(void)
{
	uint32_t cnt = 0;

	if (!ready) {
		return 0;
	}
	k_mutex_lock(&ls_lock, K_FOREVER);
	if (nvs_read(&fs, ID_COUNT, &cnt, sizeof(cnt)) != (ssize_t)sizeof(cnt)) {
		cnt = 0;
	}
	k_mutex_unlock(&ls_lock);
	return cnt;
}

int logstore_append_fault(struct fault_event *ev)
{
	if (!ready) {
		return -ENODEV;
	}
	uint32_t cnt = logstore_fault_count();

	ev->seq = cnt;
	k_mutex_lock(&ls_lock, K_FOREVER);
	ssize_t n = nvs_write(&fs, ID_FAULT0 + (cnt % FAULT_MAX), ev, sizeof(*ev));

	if (n >= 0) {
		cnt++;
		n = nvs_write(&fs, ID_COUNT, &cnt, sizeof(cnt));
	}
	k_mutex_unlock(&ls_lock);
	return (n < 0) ? (int)n : 0;
}

int logstore_read_fault(uint32_t k, struct fault_event *ev)
{
	uint32_t cnt = logstore_fault_count();

	if (k >= cnt || k >= FAULT_MAX) {
		return -ENOENT;
	}
	uint32_t seq = cnt - 1u - k;

	k_mutex_lock(&ls_lock, K_FOREVER);
	ssize_t n = nvs_read(&fs, ID_FAULT0 + (seq % FAULT_MAX), ev, sizeof(*ev));

	k_mutex_unlock(&ls_lock);
	return (n == (ssize_t)sizeof(*ev)) ? 0 : -EIO;
}

int logstore_clear_faults(void)
{
	uint32_t zero = 0;

	if (!ready) {
		return -ENODEV;
	}
	k_mutex_lock(&ls_lock, K_FOREVER);
	for (int i = 0; i < FAULT_MAX; i++) {
		(void)nvs_delete(&fs, ID_FAULT0 + i);
	}
	ssize_t n = nvs_write(&fs, ID_COUNT, &zero, sizeof(zero));

	k_mutex_unlock(&ls_lock);
	return (n < 0) ? (int)n : 0;
}
