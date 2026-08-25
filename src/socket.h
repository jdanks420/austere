#ifndef AUSTERE_SOCKET_H
#define AUSTERE_SOCKET_H

#include "wm.h"

/* §8: minimal control surface — SOCK_STREAM at
 * $XDG_RUNTIME_DIR/austere/socket, one "action [arg]" per line,
 * "ok"/"err <reason>" replies. A misbehaving client can hurt only
 * itself (§10). */

int socket_init(wm_t *wm);   /* listener fd, -1 when disabled */
int socket_fd(void);
void socket_handle(wm_t *wm); /* accept + serve pending connections */
void socket_shutdown(wm_t *wm);

#endif
