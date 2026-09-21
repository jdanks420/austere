#include <string.h>

#include "barmod.h"

/* One row per built-in module, same contract as the layout registry
 * (SPEC §6.2); the regs themselves live in bar_modules/<name>.c. */
extern const mod_reg_t wsmod, layoutmod, titlemod;
extern const mod_reg_t clockmod, batmod, volmod;
extern const mod_reg_t cpumod, rammod;

const mod_reg_t *const bar_module_reg[] = {
    &wsmod, &layoutmod, &titlemod, &clockmod, &batmod, &volmod,
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
