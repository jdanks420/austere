#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bar.h"
#include "settings.h"
#include "draw.h"
#include "monitor.h"
#include "popup.h"
#include "util.h"

#define POPUP_TIMEOUT_MS (cfg.popup_timeout * 1000)
#define POPUP_PAD 6
#define POPUP_W 320

typedef struct {
    char text[256];
    long deadline;
} popup_entry_t;

/* One region per notification site: shown over the focus monitor.
 * Pass-through input (no event mask), override-redirect. */
static xcb_window_t win;
static draw_t draw;
static bool mapped;
static popup_entry_t queue[POPUP_DEPTH];
static unsigned nqueue;

static long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void
popups_init(wm_t *wm)
{
    (void)wm;
    win = XCB_NONE;
    mapped = false;
    nqueue = 0;
}

void
popups_sync(wm_t *wm)
{
    if (win != XCB_NONE) {
        xcb_free_gc(wm->conn, draw.gc);
        xcb_destroy_window(wm->conn, win);
        win = XCB_NONE;
        mapped = false;
    }
}

static void
popup_ensure_window(wm_t *wm)
{
    Rect a = mon_workarea(focused_mon(wm));
    font_t *f = draw_ui_font(wm);
    unsigned h = font_height(f) + 2 * POPUP_PAD;

    win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, win,
        wm->scr->root,
        (int16_t)(a.x + a.w - POPUP_W - POPUP_PAD),
        (int16_t)(a.y + POPUP_PAD), POPUP_W, h, 1,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
        (uint32_t[]){ 0x202020, 1, 0 });
    draw_setup(wm, &draw, win);
    mapped = false;
}

static void
popup_draw(wm_t *wm)
{
    const char *s = queue[nqueue - 1].text;
    font_t *f = draw_ui_font(wm);
    unsigned h = font_height(f) + 2 * POPUP_PAD;

    draw_rect(wm, &draw, 0, 0, POPUP_W, h, 0x202020);
    draw_text(wm, &draw, f, POPUP_PAD,
        (int)(POPUP_PAD + font_ascent(f)), s, (unsigned)strlen(s),
        0xdddddd, 0x202020);
}

void
popup_notify(wm_t *wm, const char *fmt, ...)
{
    va_list ap;

    if (nqueue == POPUP_DEPTH)
        memmove(queue, queue + 1, (POPUP_DEPTH - 1) * sizeof(queue[0]));
    else
        nqueue++;
    va_start(ap, fmt);
    vsnprintf(queue[nqueue - 1].text, sizeof(queue[0].text), fmt, ap);
    va_end(ap);
    queue[nqueue - 1].deadline = now_ms() + POPUP_TIMEOUT_MS;

    fprintf(stderr, "austere: %s\n", queue[nqueue - 1].text);

    if (win == XCB_NONE)
        popup_ensure_window(wm);
    if (!mapped) {
        xcb_map_window(wm->conn, win);
        mapped = true;
    }
    /* Draw after map: the server clears the window on map and would
     * wipe anything painted first. */
    popup_draw(wm);
    /* Raise above everything, including bars. */
    uint32_t vals = XCB_STACK_MODE_ABOVE;

    xcb_configure_window(wm->conn, win, XCB_CONFIG_WINDOW_STACK_MODE,
        &vals);
}

int
popups_timeout_ms(wm_t *wm)
{
    (void)wm;

    if (!nqueue)
        return -1;
    long left = queue[0].deadline - now_ms();

    return left > 0 ? (int)left : 0;
}

void
popups_tick(wm_t *wm)
{
    bool expired = false;

    while (nqueue && queue[0].deadline <= now_ms()) {
        memmove(queue, queue + 1, (POPUP_DEPTH - 1) * sizeof(queue[0]));
        nqueue--;
        expired = true;
    }
    if (expired && !nqueue && win != XCB_NONE) {
        xcb_free_gc(wm->conn, draw.gc);
        xcb_destroy_window(wm->conn, win);
        win = XCB_NONE;
        mapped = false;
    }
}

void
popups_shutdown(wm_t *wm)
{
    if (win != XCB_NONE)
        xcb_destroy_window(wm->conn, win);
    win = XCB_NONE;
}
