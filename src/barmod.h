#ifndef AUSTERE_BARMOD_H
#define AUSTERE_BARMOD_H

#include <stdbool.h>

#include "bar.h"

/* Render draws (or, with draw=false, only measures) the module at x
 * and returns the width it occupies. Click receives the module's
 * origin plus the press position in window coordinates. */
typedef struct {
    const char *type;
    unsigned (*render)(wm_t *wm, struct monitor *mon, module_t *m,
        int x, bool draw);
    void (*click)(wm_t *wm, struct monitor *mon, module_t *m, int mod_x,
        int px, unsigned btn);
} mod_reg_t;

extern const mod_reg_t *const bar_module_reg[];
const mod_reg_t *mod_lookup(const char *type);

#endif
