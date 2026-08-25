#ifndef AUSTERE_BAR_H
#define AUSTERE_BAR_H

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "draw.h"
#include "monitor.h"
#include "wm.h"

/* Hardcoded until M7 wires [bar] conf sections. */
#define BAR_FRAME_MAX 4096
#define BAR_MAX_MODULES 16

#define BAR_BG 0x1a1a1a
#define BAR_FG 0xcccccc
#define BAR_DIM 0x777777
#define BAR_ACCENT 0x5f819d
#define BAR_URGENT 0xaf3f3f
#define BAR_PAD 4

typedef struct module {
    char name[32];
    const char *type; /* built-in type, NULL for script modules */
    char *exec;       /* script module command */
    font_t *font;
    uint32_t color;
    char frame[BAR_FRAME_MAX];
    size_t flen;
    pid_t pid;
    int fd;
    size_t blen; /* partial-line buffer fill */
    bool dead;
} module_t;

typedef struct bar {
    xcb_window_t win;
    draw_t draw;
    unsigned height;
    bool mapped;
    module_t mods[BAR_MAX_MODULES];
    unsigned nmods;  /* total instances */
    unsigned nleft;  /* instances[0..nleft) form the left group */
} bar_t;


/* Shared helpers for built-in module renderers. */
font_t *bar_font(wm_t *wm, module_t *m);
uint32_t bar_color(module_t *m, uint32_t fallback);
int bar_baseline(wm_t *wm, struct monitor *mon, font_t *f);

void bar_init(wm_t *wm);
void bars_sync(wm_t *wm); /* topology changed: create/resize/destroy */
void bar_render(wm_t *wm, monitor_t *mon);
void bar_render_all(wm_t *wm);
Rect mon_workarea(const monitor_t *m);
int bar_timeout_ms(wm_t *wm);
bool bar_button(wm_t *wm, xcb_window_t win, int x, unsigned btn);
void bar_pump_fd(wm_t *wm, int fd);
void bar_collect_fds(wm_t *wm, struct pollfd *fds, unsigned *n,
    unsigned max);
void bar_teardown(wm_t *wm, monitor_t *m);
void bars_shutdown(wm_t *wm);

#endif
