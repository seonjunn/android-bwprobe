/*
 * pmu.h — PMU perf_event helpers
 *
 * open_evt_raw()   : open any (type, config) perf_event counter
 * open_bus_access(): open bus_access (L2 miss, reads+writes) counter
 *
 * exclude_kernel=0 is MANDATORY on Oryon V2: cache refills are attributed to
 * kernel context, causing 23-33x undercounting with exclude_kernel=1.
 */

#pragma once
#include <string.h>
#include "cpu.h"

/*
 * Open a raw perf_event counter for the given (type, config) pair.
 * exclude_kernel=0 is MANDATORY on Oryon V2: cache refills are attributed
 * to kernel context, causing 23-33x undercounting with exclude_kernel=1.
 * Returns fd on success, -1 on failure.
 */
static inline int open_evt_raw(uint32_t type, uint64_t config)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type           = type;
    a.config         = config;
    a.size           = sizeof(a);
    a.disabled       = 1;
    a.exclude_kernel = 0;   /* MANDATORY on Oryon V2 */
    a.exclude_hv     = 1;
    return (int)perf_event_open(&a, 0, -1, -1, 0);
}

/* Open bus_access (config=0x0019) on armv8_pmuv3.  Returns fd or -1. */
static inline int open_bus_access(void)
{
    return open_evt_raw(ARMV8_PMU_TYPE, 0x0019);
}
