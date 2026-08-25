#ifndef AUSTERE_UTIL_H
#define AUSTERE_UTIL_H

#include <stdbool.h>
#include <xcb/xcb.h>
#include "wm.h"

void *xmalloc(size_t n);
int mkdir_p(const char *dir);
char *xstrdup(const char *s);

/* Validated property read: returns malloc'd raw bytes and count of
 * 32-bit-format units implied by format, or NULL if absent/mismatched. */
void *get_property(wm_t *wm, xcb_window_t win, xcb_atom_t prop,
    xcb_atom_t type, uint32_t expect_format, size_t *out_len);
uint8_t *get_string_property(wm_t *wm, xcb_window_t win, xcb_atom_t prop,
    size_t *out_len);
bool has_proto(wm_t *wm, xcb_window_t win, xcb_atom_t proto);

void spawn_async(char *const argv[]);
void spawn_shell(const char *cmd);

#endif
