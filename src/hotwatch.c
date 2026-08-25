#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include "hotwatch.h"
#include "util.h"

#define DEBOUNCE_MS 100

static int ifd = -1;
static int wd = -1;
static char watched[512];
static long deadline;
static bool pending;

static long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
add_watch(void)
{
    if (wd >= 0)
        inotify_rm_watch(ifd, wd);
    wd = inotify_add_watch(ifd, watched,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
}

int
hotwatch_init(const char *path)
{
    ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);

    if (ifd < 0)
        return -1;
    snprintf(watched, sizeof(watched), "%s", path);
    add_watch();
    return ifd;
}

int
hotwatch_fd(void)
{
    return ifd;
}

void
hotwatch_pump(void)
{
    char buf[1024] __attribute__((aligned(8)));
    bool saw = false;

    for (;;) {
        ssize_t r = read(ifd, buf, sizeof(buf));

        if (r <= 0)
            break;
        for (ssize_t off = 0; off + (ssize_t)sizeof(struct inotify_event) <= r;) {
            const struct inotify_event *ev =
                (const struct inotify_event *)(buf + off);

            if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO |
                    IN_DELETE_SELF | IN_MOVE_SELF))
                saw = true;
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF |
                    IN_IGNORED)) {
                wd = -1; /* editor swapped the file; re-arm on fire */
            }
            off += (ssize_t)sizeof(struct inotify_event) + ev->len;
        }
    }
    if (saw) {
        pending = true;
        deadline = now_ms() + DEBOUNCE_MS;
    }
}

int
hotwatch_timeout_ms(void)
{
    if (!pending)
        return -1;
    long left = deadline - now_ms();

    return left > 0 ? (int)left : 0;
}

bool
hotwatch_fire(void)
{
    if (!pending || now_ms() < deadline)
        return false;
    pending = false;
    if (wd < 0)
        add_watch();
    return true;
}
