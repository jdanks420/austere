#ifndef AUSTERE_HOTWATCH_H
#define AUSTERE_HOTWATCH_H

#include <stdbool.h>

#include "wm.h"

/* §9.2: inotify watch on austere.conf (CLOSE_WRITE | MOVED_TO covers
 * editors that save via rename), ~100 ms debounce, poll-set fd. */

int hotwatch_init(const char *path); /* inotify fd, -1 on failure */
int hotwatch_fd(void);               /* poll-set fd */
void hotwatch_pump(void);            /* drain events, arm the debounce */
int hotwatch_timeout_ms(void);       /* ms until reload, or -1 */
bool hotwatch_fire(void);            /* true when a reload is due */

#endif
