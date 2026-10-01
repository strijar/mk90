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
#include <time.h>
#include <unistd.h>

static struct {
    lv_display_t    *display;
    lv_indev_t      *pointer;
    Display         *connection;
    Window           window;
    Atom             delete_atom;
    lv_point_t       position;
    bool             pressed, closing;
    mk90_host_key_cb key;
    unsigned long    held[256];
} host;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    (void) indev;
    data->point = host.position;
    data->state = host.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void deleted(lv_event_t *e) {
    (void) e;
    host.display = NULL;
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

lv_display_t *mk90_x11_open(mk90_host_key_cb callback) {
    memset(&host, 0, sizeof(host));

    /* LVGL's driver assumes XOpenDisplay succeeds. Fail cleanly beforehand. */

    Display *probe = XOpenDisplay(NULL);

    if (!probe) {
        fprintf(stderr, "Cannot open X11 display; check DISPLAY or use --headless.\n");
        return NULL;
    }

    XCloseDisplay(probe);
    host.key = callback;
    host.display = lv_x11_window_create("MK-90", 790, 312);

    if (!host.display)
        return NULL;

    _x11_user_hdr_t *header = lv_display_get_driver_data(host.display);
    host.connection = header->display;

    XEvent event;

    XIfEvent(host.connection, &event, mapped, NULL);
    host.window = event.xmap.window;

    unsigned long pid = (unsigned long) getpid();

    XChangeProperty(host.connection, host.window, XInternAtom(host.connection, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *) &pid, 1);
    XPutBackEvent(host.connection, &event);
    XSelectInput(host.connection, host.window, PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask | FocusChangeMask);
    XUndefineCursor(host.connection, host.window);

    Bool supported;

    XkbSetDetectableAutoRepeat(host.connection, True, &supported);
    host.delete_atom = XInternAtom(host.connection, "WM_DELETE_WINDOW", False);
    host.pointer = lv_indev_create();

    lv_indev_set_type(host.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(host.pointer, host.display);
    lv_indev_set_read_cb(host.pointer, read_pointer);
    lv_display_add_event_cb(host.display, deleted, LV_EVENT_DELETE, NULL);

    return host.display;
}

bool mk90_x11_poll(void) {
    if (!host.display || host.closing)
        return false;

    XEvent event;

    while (XCheckIfEvent(host.connection, &event, input, NULL)) {
        switch (event.type) {
            case KeyPress: {
                unsigned code = event.xkey.keycode & 255;
                if (!host.held[code]) {
                    KeySym symbol;
                    char   text[16];

                    XLookupString(&event.xkey, text, sizeof(text), &symbol, NULL);
                    host.held[code] = symbol;
                    host.key(symbol, true);
                }
                break;
            }

            case KeyRelease: {
                /* Fallback for servers without detectable autorepeat. */
                if (XPending(host.connection)) {
                    XEvent next;

                    XPeekEvent(host.connection, &next);

                    if (next.type == KeyPress && next.xkey.keycode == event.xkey.keycode &&
                        next.xkey.time == event.xkey.time)
                        break;
                }

                unsigned code = event.xkey.keycode & 255;

                if (host.held[code])
                    host.key(host.held[code], false);

                host.held[code] = 0;
                break;
            }

            case MotionNotify:
                host.position = (lv_point_t) { event.xmotion.x, event.xmotion.y };
                break;

            case ButtonPress:
            case ButtonRelease:
                host.position = (lv_point_t) { event.xbutton.x, event.xbutton.y };

                if (event.xbutton.button == Button1) {
                    host.pressed = event.type == ButtonPress;
                    lv_indev_read(host.pointer);
                }
                break;

            case FocusOut:
                for (unsigned i = 0; i < 256; i++)
                    if (host.held[i]) {
                        host.key(host.held[i], false);
                        host.held[i] = 0;
                    }

                host.pressed = false;
                lv_indev_read(host.pointer);
                break;

            case ClientMessage:
                if (event.xclient.data.l[0] == (long) host.delete_atom)
                    host.closing = true;
                break;
        }
    }
    return !host.closing;
}

void mk90_x11_close(void) {
    if (!host.display)
        return;

    lv_indev_delete(host.pointer);

    /* Let the driver stop and join its tick thread before freeing the display. */

    XEvent event = { 0 };

    event.xclient.type = ClientMessage;
    event.xclient.window = host.window;
    event.xclient.message_type = XInternAtom(host.connection, "WM_PROTOCOLS", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long) host.delete_atom;

    XSendEvent(host.connection, host.window, False, NoEventMask, &event);
    XFlush(host.connection);

    while (host.display) {
        lv_timer_handler();
        struct timespec delay = { 0, 1000000 };
        nanosleep(&delay, NULL);
    }
}
