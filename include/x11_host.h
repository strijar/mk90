#ifndef MK90_X11_HOST_H
#define MK90_X11_HOST_H

#include <lvgl.h>

typedef void (*mk90_host_key_cb)(unsigned long symbol, bool down);

lv_display_t *mk90_x11_open(mk90_host_key_cb callback);
bool          mk90_x11_poll(void);
void          mk90_x11_close(void);

#endif
