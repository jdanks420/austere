#include <string.h>

#include "barmod.h"

/* One row per built-in module, same contract as the layout registry
 * (SPEC §6.2); the regs themselves live in bar_modules/<name>.c. The
 * tray row is unconditional: it is inert in an AUSTERE_NO_DBUS build
 * (an empty view measures zero), which keeps every variant's registry
 * and a stored modules_right list identical. */
extern const mod_reg_t traymod, wsmod, layoutmod, titlemod;
extern const mod_reg_t clockmod, batmod, volmod;
extern const mod_reg_t cpumod, rammod;

const mod_reg_t *const bar_module_reg[] = {
    &traymod, &wsmod, &layoutmod, &titlemod, &clockmod, &batmod, &volmod,
    &cpumod, &rammod, NULL
};

const mod_reg_t *
mod_lookup(const char *type)
{
    if (!type)
        return NULL;
    for (unsigned i = 0; bar_module_reg[i]; i++)
        if (!strcmp(bar_module_reg[i]->type, type))
            return bar_module_reg[i];
    return NULL;
}

bool
mod_owns_right_click(const mod_reg_t *reg)
{
    /* The tray is the one row with a right-click of its own: on it
     * button 3 is the indicator's context menu (SPEC §7.6), which the
     * item is asked to show, not the bar's settings menu (§6.4). Every
     * other row, and every script module, keeps the settings menu. */
    return reg == &traymod;
}
