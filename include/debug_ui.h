#ifndef MK90_DEBUG_UI_H
#define MK90_DEBUG_UI_H

#include "debugger.h"
#include <lvgl.h>

void mk90_debug_ui_init(mk90 *m, mk90_debugger *debug, lv_display_t *main_display, bool offscreen);
void mk90_debug_ui_open(void);
void mk90_debug_ui_poll(void);
void mk90_debug_ui_update(void);
void mk90_debug_ui_close(void);
bool mk90_debug_ui_visible(void);
bool mk90_debug_ui_snapshot(const char *path);

#endif
