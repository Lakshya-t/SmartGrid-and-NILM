#ifndef LOGSTORE_H_
#define LOGSTORE_H_

#include <stdbool.h>
#include "app.h"

int      logstore_init(void);
bool     logstore_load_cfg(struct app_cfg *c);      /* false if none/invalid */
int      logstore_save_cfg(const struct app_cfg *c);
int      logstore_append_fault(struct fault_event *ev);
uint32_t logstore_fault_count(void);                /* total ever written   */
/* k = 0 is the newest entry. Returns 0 on success. */
int      logstore_read_fault(uint32_t k, struct fault_event *ev);
int      logstore_clear_faults(void);

#endif
