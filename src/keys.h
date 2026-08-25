#ifndef AUSTERE_KEYS_H
#define AUSTERE_KEYS_H

#include "settings.h"
#include "wm.h"

void keys_defaults(settings_t *s); /* seed compiled-in binds */
bool parse_bind(const char *combo, const char *action, bind_t *out);
void grab_keys(wm_t *wm);
void ungrab_keys(wm_t *wm);
unsigned mod_from_name(const char *name, bool *ok);
void keysym_name(xcb_keysym_t sym, char *buf, unsigned bufsz);

#endif
