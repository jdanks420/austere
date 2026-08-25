#ifndef AUSTERE_STATE_H
#define AUSTERE_STATE_H

#include "wm.h"

/* §10 restart-safety: per-window session state serialized to
 * $XDG_RUNTIME_DIR/austere/state before execvp(self); consumed (and
 * unlinked) on the next boot whether or not it matches. */

void state_save(wm_t *wm);
void state_replay(wm_t *wm);

#endif
