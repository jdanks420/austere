#ifndef AUSTERE_STATES_H
#define AUSTERE_STATES_H

#include "wm.h"

void menu_states_open(wm_t *wm);
bool states_load(wm_t *wm, const char *name);
const char *states_boot_override(void);

#endif
