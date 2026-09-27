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
 * the returned image stays valid until process exit.
 *
 * Never use this for a name that a remote client can choose (tray
 * IconName): every distinct name, including a miss, would live in the
 * process-lifetime cache. Use icon_resolve_ex() there. */
image_t *icon_get(const char *name, unsigned target_h);

/* Same lookup and decode, but nothing is remembered: each call decodes
 * afresh and the caller owns the returned image (free img->argb, then
 * img). For a name the process chose itself, where no extra icon theme
 * path is in play - which is what separates this from icon_resolve_ex(),
 * not any difference in trust. */
image_t *icon_resolve(const char *name, unsigned target_h);

/* Same resolve-only guarantee as icon_resolve(), plus a caller-supplied
 * icon theme to search first: extra_path is a colon-separated list of
 * theme roots, as an SNI's IconThemePath arrives, and NULL or "" means
 * only the XDG data dirs. A bare name is matched as theme content
 * (index.theme Directories and Inherits) before it is tried as a
 * plain path. Still uncached, so no entry survives the call and a
 * client that picks a fresh name every time cannot grow anything.
 *
 * The returned image belongs to the resolver, not the caller: it stays
 * valid until the next icon_resolve_ex() call, so copy the pixels out
 * before doing anything else. It is const because the caller neither
 * owns nor modifies it, and must not free it or push it into
 * icon_get()'s cache.
 *
 * Remote names (an item's IconName or AttentionIconName, a menu row's
 * icon) must go through this, never icon_get() and never icon_resolve():
 * only this one takes the theme path the remote client advertised, which
 * is where such an icon usually lives. */
const image_t *icon_resolve_ex(const char *name, unsigned target_h,
    const char *extra_path);

#endif
