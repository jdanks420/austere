#ifndef AUSTERE_MODULE_H
#define AUSTERE_MODULE_H

#include <stdbool.h>

#include "bar.h"
#include "wm.h"

void module_spawn_script(wm_t *wm, module_t *m);
bool module_pump(wm_t *wm, module_t *m);
void module_kill(wm_t *wm, module_t *m);

#endif
