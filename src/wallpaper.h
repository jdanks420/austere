#ifndef AUSTERE_WALLPAPER_H
#define AUSTERE_WALLPAPER_H

#include "wm.h"

/* §7.4: austere chooses a file and delegates rendering to the
 * setter command (feh by default). The picker is a full-screen grid
 * of thumbnails (imlib2) or a text list in NO_IMLIB2 builds. */

void wallpaper_random(wm_t *wm);
void wallpaper_next(wm_t *wm);
void wallpaper_set(wm_t *wm, const char *path);
void wallpaper_pick(wm_t *wm);

#endif
