#ifndef AUSTERE_APPS_H
#define AUSTERE_APPS_H

#include "wm.h"

void menu_apps_open(wm_t *wm);
void apps_rescan(void);
unsigned apps_count(void);
const char *app_name(unsigned i);
int app_category(unsigned i);
const char *app_category_name(unsigned cat);
const char *app_exec_for(const char *name);

#endif
