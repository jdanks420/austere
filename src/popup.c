#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bar.h"
#include "draw.h"
#include "icons.h"
#include "monitor.h"
#include "popup.h"
#include "settings.h"
#include "util.h"

#define TOAST_W 360
#define TOAST_PAD 10
#define TOAST_GAP 8
#define TOAST_ICON 32
#define TOAST_BODY_LINES 3
#define TOAST_ACTION_GAP 14
#define TOAST_MAX_ACTIONS 4
#define TOAST_TEXT 512
#define TOAST_REGIONS 8
#define TOAST_LIFE_MS ((long)cfg.popup_timeout * 1000)

typedef struct {
    bool used;
    unsigned id;              /* 0 = internal WM message */
    char *app;
    char *summary;
    char *body;
    image_t *img;             /* resolved via icons.c, owned by its cache */
    int urgency;
    long deadline;            /* ms since boot; 0 = never expires */
    bool resident;
    bool invoked;             /* an action already fired for this toast */
    int mon;                  /* monitor the toast is shown on */
    toast_action_t actions[TOAST_MAX_ACTIONS];
    unsigned nactions;
} toast_t;

/* One override-redirect window per monitor holding that monitor's
 * stack. Input-enabled (unlike the old pass-through box) so toasts can
 * be clicked; focus is never taken. */
typedef struct {
    xcb_window_t win;
    draw_t draw;
    bool mapped;
} region_t;

static toast_t toasts[TOAST_MAX];
static unsigned ntoasts;
static region_t regions[TOAST_REGIONS];
static unsigned next_id = 1;
static toast_closed_fn closed_cb;
static void *closed_ud;
static toast_action_fn action_cb;
static void *action_ud;

static long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void
popups_set_closed_cb(toast_closed_fn fn, void *ud)
{
    closed_cb = fn;
    closed_ud = ud;
}

void
popups_set_action_cb(toast_action_fn fn, void *ud)
{
    action_cb = fn;
    action_ud = ud;
}

static void
toast_release(toast_t *t)
{
    free(t->app);
    free(t->summary);
    free(t->body);
    for (unsigned i = 0; i < t->nactions; i++) {
        free(t->actions[i].key);
        free(t->actions[i].label);
    }
    memset(t, 0, sizeof(*t));
}

/* ---- text helpers ---------------------------------------------------- */

/* Greedy word wrap into at most max_lines lines of max_w pixels. A word
 * wider than the line is hard-split so one long token cannot overflow.
 * Returns how many lines were written. */
static unsigned
wrap_text(wm_t *wm, font_t *f, const char *text, unsigned max_w,
    char lines[][TOAST_TEXT], unsigned max_lines)
{
    unsigned n = 0;
    size_t len = strlen(text);
    size_t pos = 0;

    while (pos < len && n < max_lines) {
        size_t take = len - pos;

        if (take > TOAST_TEXT - 1)
            take = TOAST_TEXT - 1;
        /* shrink until the remainder fits, then back up to the last
         * space inside it so words stay whole when possible */
        while (take > 1 &&
            draw_text_w(wm, f, text + pos, (unsigned)take) > max_w)
            take--;
        size_t brk = take;
        if (brk < len - pos) {
            size_t sp = brk;

            while (sp > 0 && text[pos + sp - 1] != ' ')
                sp--;
            if (sp > 0)
                brk = sp;
        }
        memcpy(lines[n], text + pos, brk);
        lines[n][brk] = '\0';
        n++;
        pos += brk;
        while (pos < len && text[pos] == ' ')
            pos++;
    }
    return n;
}

/* Blend a towards b; t=0 keeps a, t=255 reaches b. Used for dimmed
 * secondary text on an opaque surface (no alpha needed). */
static uint32_t
mix(uint32_t a, uint32_t b, unsigned t)
{
    unsigned ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    unsigned br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;

    return ((((ar * (255 - t) + br * t) / 255) & 0xff) << 16) |
        ((((ag * (255 - t) + bg * t) / 255) & 0xff) << 8) |
        ((ab * (255 - t) + bb * t) / 255);
}

/* ---- layout ---------------------------------------------------------- */

static unsigned
toast_text_w(const toast_t *t)
{
    return TOAST_W - 2 * TOAST_PAD - (t->img ? TOAST_ICON + TOAST_PAD : 0);
}

/* Body lines, computed once and reused by layout, drawing and hit
 * testing so the three can never disagree. */
static unsigned
toast_body_lines(wm_t *wm, const toast_t *t, font_t *f,
    char lines[][TOAST_TEXT])
{
    if (!t->body || !*t->body)
        return 0;
    return wrap_text(wm, f, t->body, toast_text_w(t), lines,
        TOAST_BODY_LINES);
}

static unsigned
toast_height(wm_t *wm, const toast_t *t)
{
    font_t *f = draw_ui_font(wm);
    char lines[TOAST_BODY_LINES][TOAST_TEXT];
    unsigned h = TOAST_PAD * 2;

    h += font_height(f) * 2;                 /* app name + summary */
    h += toast_body_lines(wm, t, f, lines) * font_height(f);
    if (t->nactions)
        h += 4 + font_height(f) + 4;
    return h;
}

/* ---- rendering ------------------------------------------------------- */

/* Action row geometry, shared by the draw pass and hit testing so a
 * click can never disagree with what is on screen. Boxes come back
 * right-to-left (draw order); each is {x, width, y}. */
static unsigned
toast_action_boxes(wm_t *wm, const toast_t *t, int boxes[][3])
{
    font_t *f = draw_ui_font(wm);
    char lines[TOAST_BODY_LINES][TOAST_TEXT];
    int lh = (int)font_height(f);
    int ay = (int)TOAST_PAD + lh * 2 +
        (int)toast_body_lines(wm, t, f, lines) * lh + 4;
    int ax = TOAST_W - TOAST_PAD;
    unsigned n = 0;

    for (unsigned i = t->nactions; i-- > 0 && n < TOAST_MAX_ACTIONS;) {
        int wpx = (int)draw_text_w(wm, f, t->actions[i].label,
            (unsigned)strlen(t->actions[i].label));

        ax -= wpx;
        boxes[n][0] = ax;
        boxes[n][1] = wpx;
        boxes[n][2] = ay;
        n++;
        ax -= TOAST_ACTION_GAP;
    }
    return n;
}

static void
toast_paint(wm_t *wm, region_t *rg, const toast_t *t, int yoff)
{
    font_t *f = draw_ui_font(wm);
    char lines[TOAST_BODY_LINES][TOAST_TEXT];
    unsigned lh = font_height(f);
    unsigned h = toast_height(wm, t);
    uint32_t dim = mix(cfg.bar_fg, cfg.bar_bg, 110);
    uint32_t accent = t->urgency == TOAST_URGENCY_CRITICAL
        ? cfg.urgent_color : cfg.focus_color;
    int tx = TOAST_PAD + (t->img ? TOAST_ICON + TOAST_PAD : 0);
    int y = yoff + (int)TOAST_PAD;

    draw_rect(wm, &rg->draw, 0, yoff, TOAST_W, h, cfg.bar_bg);
    draw_rect(wm, &rg->draw, 0, yoff, TOAST_W, 1, accent);
    draw_rect(wm, &rg->draw, 0, yoff + (int)h - 1, TOAST_W, 1, accent);
    draw_rect(wm, &rg->draw, 0, yoff, 1, h, accent);
    draw_rect(wm, &rg->draw, TOAST_W - 1, yoff, 1, h, accent);
    if (t->urgency == TOAST_URGENCY_CRITICAL)
        draw_rect(wm, &rg->draw, 1, yoff + 1, 2, h - 2, accent);
    if (t->img)
        draw_put_image24(wm, rg->win, rg->draw.gc, rg->draw.depth,
            TOAST_PAD, (int16_t)y, t->img->w, t->img->h, t->img->argb);

    draw_text(wm, &rg->draw, f, tx, y + (int)font_ascent(f),
        t->app, (unsigned)strlen(t->app), dim, cfg.bar_bg);
    y += (int)lh;
    draw_text(wm, &rg->draw, f, tx, y + (int)font_ascent(f),
        t->summary, (unsigned)strlen(t->summary), cfg.bar_fg, cfg.bar_bg);
    y += (int)lh;

    unsigned nb = toast_body_lines(wm, t, f, lines);
    for (unsigned i = 0; i < nb; i++) {
        draw_text(wm, &rg->draw, f, tx, y + (int)font_ascent(f),
            lines[i], (unsigned)strlen(lines[i]), dim, cfg.bar_bg);
        y += (int)lh;
    }
    if (t->nactions) {
        int boxes[TOAST_MAX_ACTIONS][3];
        unsigned n = toast_action_boxes(wm, t, boxes);

        for (unsigned i = 0; i < n; i++) {
            /* boxes are toast-relative; the window stacks them */
            draw_text(wm, &rg->draw, f, boxes[i][0],
                yoff + boxes[i][2] + (int)font_ascent(f),
                t->actions[t->nactions - 1 - i].label,
                (unsigned)strlen(t->actions[t->nactions - 1 - i].label),
                cfg.focus_color, cfg.bar_bg);
        }
    }
}

static void
region_destroy(wm_t *wm, region_t *rg)
{
    if (rg->win != XCB_NONE) {
        xcb_free_gc(wm->conn, rg->draw.gc);
        xcb_destroy_window(wm->conn, rg->win);
    }
    memset(rg, 0, sizeof(*rg));
    rg->win = XCB_NONE;
}

static monitor_t *
mon_at(wm_t *wm, int idx)
{
    int i = 0;

    for (monitor_t *m = wm->mons; m; m = m->next, i++)
        if (i == idx)
            return m;
    return focused_mon(wm);
}

static int
focused_index(wm_t *wm)
{
    monitor_t *f = focused_mon(wm);
    int i = 0;

    for (monitor_t *m = wm->mons; m; m = m->next, i++)
        if (m == f)
            return i;
    return 0;
}

static void
region_sync(wm_t *wm, int mi)
{
    if (mi < 0 || mi >= TOAST_REGIONS)
        return;
    region_t *rg = &regions[mi];
    unsigned h = 0;

    for (unsigned i = 0; i < ntoasts; i++)
        if (toasts[i].used && toasts[i].mon == mi)
            h += toast_height(wm, &toasts[i]) + TOAST_GAP;
    if (!h) {
        region_destroy(wm, rg);
        return;
    }
    monitor_t *m = mon_at(wm, mi);
    Rect a = m ? mon_workarea(m) : (Rect){ 0, 0, TOAST_W, h };
    int x = a.x + (int)a.w - TOAST_W - TOAST_PAD;
    int y = a.y + TOAST_PAD;

    if (rg->win == XCB_NONE) {
        rg->win = xcb_generate_id(wm->conn);
        xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, rg->win,
            wm->scr->root, (int16_t)x, (int16_t)y, TOAST_W, (uint16_t)h, 1,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
            XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL |
                XCB_CW_EVENT_MASK,
            (uint32_t[]){ cfg.bar_bg, 1,
                XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE });
        draw_setup(wm, &rg->draw, rg->win);
    } else
        xcb_configure_window(wm->conn, rg->win,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            (uint32_t[]){ (uint32_t)x, (uint32_t)y, TOAST_W, h });
    if (!rg->mapped) {
        xcb_map_window(wm->conn, rg->win);
        rg->mapped = true;
    }
    /* paint after map/configure: the server clears on both */
    int yoff = 0;
    for (unsigned i = 0; i < ntoasts; i++) {
        if (!toasts[i].used || toasts[i].mon != mi)
            continue;
        toast_paint(wm, rg, &toasts[i], yoff);
        yoff += (int)toast_height(wm, &toasts[i]) + TOAST_GAP;
    }
    /* Ship the toast now: the event loop's next flush can be seconds
     * away (its poll timeout is the toast's own deadline), and a
     * half-flushed batch leaves the action row unwritten. */
    xcb_flush(wm->conn);
    raise_window(wm, rg->win);
}

static void
regions_sync(wm_t *wm)
{
    for (int i = 0; i < TOAST_REGIONS; i++)
        region_sync(wm, i);
}

/* ---- stack management ------------------------------------------------ */

static void
toast_remove_at(unsigned idx, unsigned reason)
{
    if (idx >= ntoasts || !toasts[idx].used)
        return;
    unsigned id = toasts[idx].id;

    toast_release(&toasts[idx]);
    for (unsigned i = idx + 1; i < ntoasts; i++)
        toasts[i - 1] = toasts[i];
    ntoasts--;
    /* the shift leaves a duplicate of the last live toast behind; drop
     * it, or the next toast_add frees strings the survivor still owns */
    memset(&toasts[ntoasts], 0, sizeof(toasts[ntoasts]));
    if (closed_cb && id)
        closed_cb(id, reason, closed_ud);
}

static toast_t *
toast_slot_find(unsigned id)
{
    for (unsigned i = 0; i < ntoasts; i++)
        if (toasts[i].used && toasts[i].id == id)
            return &toasts[i];
    return NULL;
}

static void
toast_fill(toast_t *t, const char *app, const char *summary,
    const char *body, const char *icon, int urgency, int timeout_ms,
    bool resident, const toast_action_t *actions, unsigned nactions)
{
    unsigned id = t->id;   /* a replaced toast keeps its id */

    toast_release(t);
    t->used = true;
    t->id = id;
    t->app = xstrdup(app ? app : "");
    t->summary = xstrdup(summary ? summary : "");
    t->body = xstrdup(body ? body : "");
    if (icon && *icon)
        t->img = icon_get(icon, TOAST_ICON);
    t->urgency = urgency;
    t->resident = resident;
    /* timeout: 0 keeps the toast until the app closes it, -1 takes the
     * [notifications] default, anything else is the sender's own value */
    t->deadline = (resident || timeout_ms == 0) ? 0
        : now_ms() + (timeout_ms > 0 ? timeout_ms
          : (long)cfg.notify_timeout * 1000);
    if (nactions > TOAST_MAX_ACTIONS)
        nactions = TOAST_MAX_ACTIONS;
    for (unsigned i = 0; i < nactions; i++) {
        t->actions[i].key = xstrdup(actions[i].key);
        t->actions[i].label = xstrdup(actions[i].label);
    }
    t->nactions = nactions;
}

/* Live stack depth, honouring [notifications] max_toasts within the
 * compile-time ceiling. */
static unsigned
toast_cap(void)
{
    return cfg.notify_max < TOAST_MAX ? cfg.notify_max : TOAST_MAX;
}

unsigned
toast_add(wm_t *wm, const char *app, const char *summary, const char *body,
    const char *icon, int urgency, int timeout_ms, unsigned replaces_id,
    bool resident, const toast_action_t *actions, unsigned nactions)
{
    toast_t *slot = replaces_id ? toast_slot_find(replaces_id) : NULL;

    if (!slot) {
        /* oldest falls off the top when the stack is full */
        if (ntoasts >= toast_cap())
            toast_remove_at(0, TOAST_CLOSED_EXPIRED);
        slot = &toasts[ntoasts++];
        toast_release(slot);
        slot->id = next_id++;
    }
    toast_fill(slot, app, summary, body, icon, urgency, timeout_ms,
        resident, actions, nactions);
    slot->mon = focused_index(wm);
    regions_sync(wm);
    return slot->id;
}

void
toast_close(wm_t *wm, unsigned id, unsigned reason)
{
    for (unsigned i = 0; i < ntoasts; i++)
        if (toasts[i].used && toasts[i].id == id) {
            toast_remove_at(i, reason);
            regions_sync(wm);
            return;
        }
}

void
popup_notify(wm_t *wm, const char *fmt, ...)
{
    char text[TOAST_TEXT];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    fprintf(stderr, "austere: %s\n", text);
    toast_add(wm, "austere", text, "", NULL, TOAST_URGENCY_NORMAL,
        (int)TOAST_LIFE_MS, 0, false, NULL, 0);
}

int
popups_timeout_ms(wm_t *wm)
{
    (void)wm;
    long best = 0;

    for (unsigned i = 0; i < ntoasts; i++) {
        if (!toasts[i].used || !toasts[i].deadline)
            continue;
        if (!best || toasts[i].deadline < best)
            best = toasts[i].deadline;
    }
    if (!best)
        return -1;
    long left = best - now_ms();

    return left > 0 ? (int)left : 0;
}

void
popups_tick(wm_t *wm)
{
    bool changed = false;

    for (unsigned i = 0; i < ntoasts;) {
        if (toasts[i].used && toasts[i].deadline &&
            toasts[i].deadline <= now_ms()) {
            toast_remove_at(i, TOAST_CLOSED_EXPIRED);
            changed = true;
            continue;
        }
        i++;
    }
    if (changed)
        regions_sync(wm);
}

bool
popup_button(wm_t *wm, xcb_window_t win, int x, int y, uint8_t btn)
{
    if (btn != XCB_BUTTON_INDEX_1)
        return false;
    for (int mi = 0; mi < TOAST_REGIONS; mi++) {
        if (regions[mi].win == XCB_NONE || regions[mi].win != win)
            continue;
        int yoff = 0;
        for (unsigned i = 0; i < ntoasts; i++) {
            if (!toasts[i].used || toasts[i].mon != mi)
                continue;
            int th = (int)toast_height(wm, &toasts[i]);
            if (y >= yoff && y < yoff + th) {
                toast_t *t = &toasts[i];
                int boxes[TOAST_MAX_ACTIONS][3];
                unsigned n = toast_action_boxes(wm, t, boxes);
                int ly = y - yoff;

                for (unsigned a = 0; a < n; a++) {
                    const char *key = t->actions[t->nactions - 1 - a].key;

                    if (x < boxes[a][0] || x >= boxes[a][0] + boxes[a][1])
                        continue;
                    if (ly < boxes[a][2] || ly >= boxes[a][2] + (int)
                        font_height(draw_ui_font(wm)))
                        continue;
                    /* one action per toast: a second click just closes */
                    if (!t->invoked && action_cb && key)
                        action_cb(t->id, key, action_ud);
                    t->invoked = true;
                    if (!t->resident) {
                        toast_remove_at(i, TOAST_CLOSED_DISMISSED);
                        regions_sync(wm);
                    }
                    return true;
                }
                if (!t->resident) {
                    toast_remove_at(i, TOAST_CLOSED_DISMISSED);
                    regions_sync(wm);
                }
                return true;
            }
            yoff += th + TOAST_GAP;
        }
        return true;
    }
    return false;
}

bool
popup_expose(wm_t *wm, xcb_window_t win)
{
    for (int mi = 0; mi < TOAST_REGIONS; mi++) {
        if (regions[mi].win == XCB_NONE || regions[mi].win != win)
            continue;
        int yoff = 0;

        for (unsigned i = 0; i < ntoasts; i++) {
            if (!toasts[i].used || toasts[i].mon != mi)
                continue;
            toast_paint(wm, &regions[mi], &toasts[i], yoff);
            yoff += (int)toast_height(wm, &toasts[i]) + TOAST_GAP;
        }
        return true;
    }
    return false;
}

/* ---- lifecycle ------------------------------------------------------- */

void
popups_init(wm_t *wm)
{
    (void)wm;
    for (int i = 0; i < TOAST_REGIONS; i++)
        regions[i].win = XCB_NONE;
    ntoasts = 0;
}

void
popups_sync(wm_t *wm)
{
    for (int i = 0; i < TOAST_REGIONS; i++)
        region_destroy(wm, &regions[i]);
    regions_sync(wm);
}

void
popups_shutdown(wm_t *wm)
{
    for (int i = 0; i < TOAST_REGIONS; i++)
        region_destroy(wm, &regions[i]);
    for (unsigned i = 0; i < ntoasts; i++)
        toast_release(&toasts[i]);
    ntoasts = 0;
    closed_cb = NULL;
    closed_ud = NULL;
    action_cb = NULL;
    action_ud = NULL;
}
