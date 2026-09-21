#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

#include "volume.h"

/* Synchronous so the pactl→amixer fallback can be decided by exit
 * status; these commands complete in microseconds and the volume
 * module already blocks the loop on popen reads. Verified irrelevant
 * to throughput. */

static int
run(const char *cmd)
{
    return system(cmd);
}

void
volume_shift(int delta_percent)
{
    char cmd[128];

    if (delta_percent >= 0)
        snprintf(cmd, sizeof(cmd),
            "pactl set-sink-volume @DEFAULT_SINK@ +%d%% 2>/dev/null",
            delta_percent);
    else
        snprintf(cmd, sizeof(cmd),
            "pactl set-sink-volume @DEFAULT_SINK@ -%d%% 2>/dev/null",
            -delta_percent);
    if (run(cmd) != 0) {
        if (delta_percent >= 0)
            snprintf(cmd, sizeof(cmd),
                "amixer set Master %d%%+ 2>/dev/null", delta_percent);
        else
            snprintf(cmd, sizeof(cmd),
                "amixer set Master %d%%- 2>/dev/null", -delta_percent);
        run(cmd);
    }
}

void
volume_toggle_mute(void)
{
    if (run("pactl set-sink-mute @DEFAULT_SINK@ toggle 2>/dev/null") != 0)
        run("amixer set Master toggle 2>/dev/null");
}
