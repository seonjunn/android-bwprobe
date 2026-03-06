/*
 * thermal.h — Skin temperature helper
 *
 * Zone 53 = sys-therm-0 (skin/board surface) on Samsung Galaxy S25+.
 * Shared by bwprobe.c, matvec.c, icc_bw.c.
 */

#pragma once
#include <stdio.h>

#define SKIN_ZONE_PATH "/sys/class/thermal/thermal_zone53/temp"

static inline double read_skin_c(void)
{
    FILE *f = fopen(SKIN_ZONE_PATH, "r");
    if (!f) return -1.0;
    int raw = 0;
    fscanf(f, "%d", &raw);
    fclose(f);
    return raw / 1000.0;
}
