#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bar.h"
#include "popup.h"
#include "util.h"

/* SPEC §6.3: a script module is spawned once; its stdout pipe sits in
 * the poll set and every printed line becomes one rendered frame. The
 * script owns its cadence — austere never timers it. */

void
module_spawn_script(wm_t *wm, module_t *m)
{
    int pfd[2];
    pid_t pid;

    m->fd = -1;
    m->pid = -1;
    m->dead = true;
    if (!m->exec || pipe(pfd) < 0)
        return;
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);

    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if (pid == 0) {
        setsid();
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", m->exec, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    m->fd = pfd[0];
    m->pid = pid;
    m->dead = false;
    m->blen = 0;
    (void)wm;
    fprintf(stderr, "austere: script module '%s' spawned\n", m->name);
}

/* Drain readable output; EOF marks death: freeze the last frame, warn
 * on stderr, surface an internal popup. No auto-respawn (§6.3). */
bool
module_pump(wm_t *wm, module_t *m)
{
    bool died = false;

    char chunk[512];

    for (;;) {
        ssize_t r = read(m->fd, chunk, sizeof(chunk));

        if (r > 0) {
            for (ssize_t i = 0; i < r; i++) {
                char ch = chunk[i];

                if (ch == '\n') {
                    m->frame[m->blen] = '\0';
                    m->flen = m->blen;
                    m->blen = 0;
                    bar_render_all(wm);
                } else if (m->blen < BAR_FRAME_MAX - 1) {
                    m->frame[m->blen++] = ch;
                }
            }
            continue;
        }
        if (r == 0 || errno != EAGAIN)
            died = true;
        break;
    }
    if (died) {
        close(m->fd);
        m->fd = -1;
        if (m->pid > 0) {
            waitpid(m->pid, NULL, 0);
            m->pid = -1;
        }
        m->dead = true;
        popup_notify(wm, "module '%s' died", m->name);
    }
    return died;
}

void
module_kill(wm_t *wm, module_t *m)
{
    (void)wm;
    if (m->fd >= 0)
        close(m->fd);
    m->fd = -1;
    if (m->pid > 0) {
        kill(m->pid, SIGTERM);
        waitpid(m->pid, NULL, 0);
        m->pid = -1;
    }
    free(m->exec);
    m->exec = NULL;
}
