#ifndef AUSTERE_CONF_H
#define AUSTERE_CONF_H

#include <stdbool.h>

#include "settings.h"
#include "wm.h"

const char *conf_path(void);
bool conf_ensure(const char *path);
bool conf_load(const char *path, settings_t *s, bool strict);
bool conf_write(const char *path, const settings_t *s);
bool conf_parse_color(const char *s, uint32_t *out);
void settings_reload(wm_t *wm);

#endif
