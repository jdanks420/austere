#ifndef AUSTERE_ACTIONS_H
#define AUSTERE_ACTIONS_H

#include "event.h"
#include "wm.h"

/* Action registry (SPEC §9.1): conf keybinds, the command socket and
 * the settings menu all reference actions by name; this table is the
 * single mapping into the ACT_* ids the dispatcher executes. */
typedef struct {
    const char *name;
    uint8_t id;
} action_ent_t;

uint8_t action_lookup(const char *name);
const char *action_name(uint8_t id);
void run_action(wm_t *wm, uint8_t action);

#endif
