#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "actions.h"
#include "launcher.h"
#include "monitor.h"
#include "socket.h"
#include "states.h"
#include "settings.h"
#include "util.h"
#include "workspace.h"

#define SOCK_BACKLOG 8

static int listener = -1;
static char sock_path[512];

int
socket_fd(void)
{
    return listener;
}

void
socket_shutdown(wm_t *wm)
{
    (void)wm;
    if (listener >= 0)
        close(listener);
    listener = -1;
    unlink(sock_path);
}

int
socket_init(wm_t *wm)
{
    if (!cfg.socket)
        return -1;
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char dir[512];

    int n = xdg && *xdg
        ? snprintf(dir, sizeof(dir), "%s/austere", xdg)
        : snprintf(dir, sizeof(dir), "/tmp/austere-%u",
            (unsigned)getuid());

    if (n < 0 || n >= (int)sizeof(dir) - 8)
        return -1;
    mkdir_p(dir);
    if (snprintf(sock_path, sizeof(sock_path), "%s/socket", dir) >=
        (int)sizeof(sock_path))
        return -1;
    unlink(sock_path); /* stale from a crashed session */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    struct sockaddr_un sa = { 0 };

    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        chmod(sock_path, 0600) < 0 || listen(fd, SOCK_BACKLOG) < 0) {
        close(fd);
        unlink(sock_path);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    listener = fd;
    (void)wm;
    return fd;
}

/* Dispatch one command line; write the reply. Returns false on a
 * malformed request that still deserves an "err" reply — never a
 * reason to drop the connection mid-line (§10 socket-safety). */
static void
serve_line(wm_t *wm, int fd, char *line)
{
    char reply[256] = "ok";

    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line) {
        dprintf(fd, "err empty\n");
        return;
    }
    char *sp = strchr(line, ' ');
    const char *arg = "";

    if (sp) {
        *sp = '\0';
        arg = sp + 1;
    }
    uint8_t id = action_lookup(line);

    if (id == 255) {
        /* socket-only verbs */
        if (!strcmp(line, "exec")) {
            if (!*arg) {
                dprintf(fd, "err exec needs a command\n");
                return;
            }
            spawn_shell(arg);
            dprintf(fd, "ok\n");
            return;
        }
        if (!strcmp(line, "state") && !strcmp(arg, "dump")) {
            dprintf(fd,
                "ok clients=%u ws=%u\n", wm->nclients,
                focused_mon(wm) ? focused_mon(wm)->ws_visible : 0);
            return;
        }
        snprintf(reply, sizeof(reply), "err unknown action '%s'",
            line);
        dprintf(fd, "%s\n", reply);
        return;
    }
    if (id == ACT_RESTART) {
        /* reply first, then tear the socket down before exec — the
         * fresh image would otherwise inherit this connection and the
         * client would block on a reply nobody owns */
        dprintf(fd, "ok\n");
        close(fd);
        socket_shutdown(wm);
        run_action(wm, id);
        return;
    }
    if (id == ACT_LOAD_STATE) {
        if (!*arg) {
            dprintf(fd, "err load_state needs a name\n");
            return;
        }
        states_load(wm, arg)
            ? dprintf(fd, "ok\n")
            : dprintf(fd, "err no such state\n");
        return;
    }
    if (id == ACT_VIEW_WS || id == ACT_SEND_WS) {
        char *end;
        long v = strtol(arg, &end, 10);

        if (*arg < '0' || *end || v < 0 || v >= WS_MAX) {
            dprintf(fd, "err %s needs index 0..%u\n", line, WS_MAX - 1);
            return;
        }
        if (id == ACT_VIEW_WS)
            view_ws(wm, (unsigned)v);
        else
            send_focused_to_ws(wm, (unsigned)v);
        dprintf(fd, "ok\n");
        return;
    }
    run_action(wm, id);
    dprintf(fd, "ok\n");
}

void
socket_handle(wm_t *wm)
{
    for (;;) {
        int fd = accept(listener, NULL, NULL);

        if (fd < 0)
            return;
        fcntl(fd, F_SETFL, O_NONBLOCK);

        /* one command batch per connection: read until EOF or a full
         * line without terminator, reply per line */
        char buf[1024];
        size_t fill = 0;
        ssize_t r;

        while ((r = read(fd, buf + fill, sizeof(buf) - fill)) > 0) {
            fill += (size_t)r;
            char *start = buf;

            for (;;) {
                char *nl = memchr(start, '\n',
                    (size_t)(buf + fill - start));

                if (!nl)
                    break;
                *nl = '\0';
                serve_line(wm, fd, start);
                start = nl + 1;
            }
            memmove(buf, start, (size_t)(buf + fill - start));
            fill = (size_t)(buf + fill - start);
            if (fill == sizeof(buf))
                break; /* garbage flood: drop the rest (§10) */
        }
        if (fill)
            serve_line(wm, fd, buf);
        close(fd);
    }
}
