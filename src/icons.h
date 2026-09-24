#ifndef AUSTERE_ICONS_H
#define AUSTERE_ICONS_H

#include "draw.h"

#define XDG_DIRS_MAX 32
#define XDG_DIR_LEN 512

/* $XDG_DATA_HOME, each $XDG_DATA_DIRS entry, plus the flatpak export
 * dir (frequently outside XDG_DATA_DIRS). First entry has priority:
 * user data overrides system data, as the XDG spec requires. Fills
 * dirs[0..n-1] and stores the count in *n. */
void xdg_data_dirs(char dirs[][XDG_DIR_LEN], unsigned *n);

/* Resolve an icon name (or absolute path) and decode it, scaled to
 * target_h pixels tall. Cached by name for the session, so repeated
 * lookups (launcher rows, notification toasts) decode once. Returns
 * NULL when the icon is missing or Imlib2 support is compiled out;
 * the returned image stays valid until process exit. */
image_t *icon_get(const char *name, unsigned target_h);

#endif
