#ifndef AUSTERE_EWMH_H
#define AUSTERE_EWMH_H

#include "wm.h"

struct client;
typedef struct client client_t;

void ewmh_init(wm_t *wm);
void ewmh_update_client_list(wm_t *wm);
void ewmh_update_wm_desktop(wm_t *wm, client_t *c);
void ewmh_update_current_desktop(wm_t *wm);
void ewmh_update_workarea(wm_t *wm);
void ewmh_set_demands_attention(wm_t *wm, client_t *c, bool on);
void ewmh_set_fullscreen(wm_t *wm, struct client *c, bool on);

#endif
