/* Input bridge for the LVGL 9.6.0 X11 display driver.
 * Its stock keypad synthesizes text presses and drops key releases/F-keys.
 * This bridge consumes raw input; LVGL still owns rendering and the window.
 * _x11_user_hdr_t is the only driver-internal dependency, isolated here.
 */

#define _POSIX_C_SOURCE 200809L

#include "x11_host.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct mk90_x11_window {
    lv_display_t    *display;
    lv_indev_t      *pointer;
    Display         *connection;
    Window           window;
    Atom             delete_atom;
    lv_point_t       position;
    bool             pressed, closing;
    mk90_host_key_cb key;
    unsigned long    held[256];
    bool             repeat;
};
static mk90_x11_window *main_window;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    mk90_x11_window *h = lv_indev_get_user_data(indev);
    data->point = h->position;
    data->state = h->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void deleted(lv_event_t *e) {
    mk90_x11_window *h = lv_event_get_user_data(e);
    h->display = NULL;
}

static int mapped(Display *d, XEvent *e, XPointer p) {
    (void) d;
    (void) p;
    return e->type == MapNotify;
}

static int input(Display *d, XEvent *e, XPointer p) {
    (void) d;
    (void) p;

    return e->type == KeyPress || e->type == KeyRelease || e->type == MotionNotify ||
           e->type == ButtonPress || e->type == ButtonRelease || e->type == FocusOut ||
           e->type == FocusIn || e->type == ClientMessage;
}

mk90_x11_window *mk90_x11_create(const char *title, unsigned width, unsigned height, mk90_host_key_cb callback, bool repeat) {
    mk90_x11_window *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->repeat = repeat;

    /* LVGL's driver assumes XOpenDisplay succeeds. Fail cleanly beforehand. */

    Display *probe = XOpenDisplay(NULL);

    if (!probe) {
        fprintf(stderr, "Cannot open X11 display; check DISPLAY or use --headless.\n");
        free(h);
        return NULL;
    }

    XCloseDisplay(probe);
    h->key = callback;
    h->display = lv_x11_window_create(title, width, height);

    if (!h->display) {
        free(h);
        return NULL;
    }

    _x11_user_hdr_t *header = lv_display_get_driver_data(h->display);
    h->connection = header->display;

    XEvent event;

    XIfEvent(h->connection, &event, mapped, NULL);
    h->window = event.xmap.window;

    unsigned long pid = (unsigned long) getpid();

    XChangeProperty(h->connection, h->window, XInternAtom(h->connection, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *) &pid, 1);
    XPutBackEvent(h->connection, &event);
    XSelectInput(h->connection, h->window, PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask | FocusChangeMask);
    XUndefineCursor(h->connection, h->window);

    Bool supported;

    XkbSetDetectableAutoRepeat(h->connection, True, &supported);
    h->delete_atom = XInternAtom(h->connection, "WM_DELETE_WINDOW", False);
    h->pointer = lv_indev_create();

    lv_indev_set_type(h->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(h->pointer, h);
    lv_indev_set_display(h->pointer, h->display);
    lv_indev_set_read_cb(h->pointer, read_pointer);
    lv_display_add_event_cb(h->display, deleted, LV_EVENT_DELETE, h);

    return h;
}

bool mk90_x11_poll_window(mk90_x11_window *h) {
    if (!h || !h->display || h->closing)
        return false;

    XEvent event;

    while (XCheckIfEvent(h->connection, &event, input, NULL)) {
        switch (event.type) {
            case KeyPress: {
                unsigned code = event.xkey.keycode & 255;
                if (!h->held[code] || h->repeat) {
                    KeySym symbol;
                    char   text[16];

                    XLookupString(&event.xkey, text, sizeof(text), &symbol, NULL);
                    h->held[code] = symbol;
                    h->key(symbol, true);
                }
                break;
            }

            case KeyRelease: {
                /* Fallback for servers without detectable autorepeat. */
                if (XPending(h->connection)) {
                    XEvent next;

                    XPeekEvent(h->connection, &next);

                    if (next.type == KeyPress && next.xkey.keycode == event.xkey.keycode &&
                        next.xkey.time == event.xkey.time)
                        break;
                }

                unsigned code = event.xkey.keycode & 255;

                if (h->held[code])
                    h->key(h->held[code], false);

                h->held[code] = 0;
                break;
            }

            case MotionNotify:
                h->position = (lv_point_t) { event.xmotion.x, event.xmotion.y };
                break;

            case ButtonPress:
            case ButtonRelease:
                h->position = (lv_point_t) { event.xbutton.x, event.xbutton.y };

                if (event.xbutton.button == Button1) {
                    h->pressed = event.type == ButtonPress;
                    lv_indev_read(h->pointer);
                }
                break;

            case FocusOut:
                for (unsigned i = 0; i < 256; i++)
                    if (h->held[i]) {
                        h->key(h->held[i], false);
                        h->held[i] = 0;
                    }

                h->pressed = false;
                lv_indev_read(h->pointer);
                break;

            case ClientMessage:
                if (event.xclient.data.l[0] == (long) h->delete_atom)
                    h->closing = true;
                break;
        }
    }
    return !h->closing;
}

void mk90_x11_destroy(mk90_x11_window *h) {
    if (!h)
        return;
    if (!h->display) {
        free(h);
        return;
    }

    lv_indev_delete(h->pointer);

    /* Let the driver stop and join its tick thread before freeing the display. */

    XEvent event = { 0 };

    event.xclient.type = ClientMessage;
    event.xclient.window = h->window;
    event.xclient.message_type = XInternAtom(h->connection, "WM_PROTOCOLS", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long) h->delete_atom;

    XSendEvent(h->connection, h->window, False, NoEventMask, &event);
    XFlush(h->connection);

    while (h->display) {
        lv_timer_handler();
        struct timespec delay = { 0, 1000000 };
        nanosleep(&delay, NULL);
    }
    free(h);
}

lv_display_t *mk90_x11_display(mk90_x11_window *h) {
    return h ? h->display : NULL;
}
lv_display_t *mk90_x11_open(mk90_host_key_cb callback) {
    main_window = mk90_x11_create("MK-90", 790, 312, callback, false);
    return mk90_x11_display(main_window);
}
bool mk90_x11_poll(void) {
    return mk90_x11_poll_window(main_window);
}
void mk90_x11_close(void) {
    mk90_x11_destroy(main_window);
    main_window = NULL;
}
