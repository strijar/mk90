/* Explicit integration test: opens its own emulator window on DISPLAY.
 * Not in the default CTest suite, which remains usable without an X server.
 */
#define _POSIX_C_SOURCE 200809L
#include "mk90.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void delay(unsigned ms) {
    struct timespec t = { ms / 1000, (long) (ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}
static Window find_window(Display *d, Window root, pid_t pid) {
    Atom           type;
    int            format;
    unsigned long  count, remaining;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, root, XInternAtom(d, "_NET_WM_PID", False), 0, 1, False, XA_CARDINAL, &type, &format, &count, &remaining, &data) == Success && data) {
        bool match = format == 32 && count == 1 && *(unsigned long *) data == (unsigned long) pid;
        XFree(data);
        if (match)
            return root;
    }
    Window   actual, parent, *children = NULL, found = 0;
    unsigned n;
    if (XQueryTree(d, root, &actual, &parent, &children, &n)) {
        for (unsigned i = 0; i < n && !found; i++)
            found = find_window(d, children[i], pid);
        if (children)
            XFree(children);
    }
    return found;
}
static void send_key(Display *d, Window w, KeySym sym, bool down) {
    XEvent e = { 0 };
    e.xkey.type = down ? KeyPress : KeyRelease;
    e.xkey.display = d;
    e.xkey.window = w;
    e.xkey.root = DefaultRootWindow(d);
    e.xkey.same_screen = True;
    e.xkey.keycode = XKeysymToKeycode(d, sym);
    XSendEvent(d, w, False, down ? KeyPressMask : KeyReleaseMask, &e);
    XFlush(d);
}
static void click(Display *d, Window w, int x, int y) {
    XEvent e = { 0 };
    e.xbutton.type = ButtonPress;
    e.xbutton.display = d;
    e.xbutton.window = w;
    e.xbutton.root = DefaultRootWindow(d);
    e.xbutton.same_screen = True;
    e.xbutton.button = Button1;
    e.xbutton.x = x;
    e.xbutton.y = y;
    XSendEvent(d, w, False, ButtonPressMask, &e);
    XFlush(d);
    delay(100);
    e.xbutton.type = ButtonRelease;
    XSendEvent(d, w, False, ButtonReleaseMask, &e);
    XFlush(d);
    delay(200);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: test_x11 /path/to/mk90\n");
        return 2;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fputs("No X11 display\n", stderr);
        return 1;
    }
    char temp[] = "/tmp/mk90-x11-XXXXXX";
    assert(mkdtemp(temp));
    char pbm[128], ppm[128];
    snprintf(pbm, sizeof(pbm), "%s/lcd.pbm", temp);
    snprintf(ppm, sizeof(ppm), "%s/window.ppm", temp);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        execl(argv[1], argv[1], "--seconds", "12", "--no-save", "--lcd", pbm, "--screenshot", ppm, (char *) NULL);
        _exit(127);
    }
    Window w = 0;
    for (unsigned i = 0; i < 200 && !w; i++) {
        delay(20);
        w = find_window(d, DefaultRootWindow(d), child);
    }
    if (!w) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        fputs("Emulator window not found\n", stderr);
        return 1;
    }
    delay(1800);
    /* Physical Enter starts BASIC. Then F5 returns to the boot menu. */
    send_key(d, w, XK_Return, true);
    delay(120);
    send_key(d, w, XK_Return, false);
    delay(700);
    send_key(d, w, XK_F5, true);
    delay(50);
    send_key(d, w, XK_F5, false);
    delay(1700);
    /* Resize and select BASIC through the on-screen VK key at 2x scale. */
    XResizeWindow(d, w, 1580, 624);
    XFlush(d);
    delay(200);
    click(d, w, 1498, 472);
    delay(1200);
    /* F2 overlay should toggle twice without affecting emulation. */
    for (unsigned i = 0; i < 2; i++) {
        send_key(d, w, XK_F2, true);
        delay(50);
        send_key(d, w, XK_F2, false);
        delay(50);
    }
    /* Lose focus with a held key; the emulator must release it. */
    send_key(d, w, XK_a, true);
    delay(80);
    XEvent focus = { 0 };
    focus.xfocus.type = FocusOut;
    focus.xfocus.display = d;
    focus.xfocus.window = w;
    XSendEvent(d, w, False, FocusChangeMask, &focus);
    XFlush(d);
    delay(150);
    send_key(d, w, XK_BackSpace, true);
    delay(80);
    send_key(d, w, XK_BackSpace, false);
    delay(250);
    XEvent close = { 0 };
    close.xclient.type = ClientMessage;
    close.xclient.window = w;
    close.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", False);
    close.xclient.format = 32;
    close.xclient.data.l[0] = (long) XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSendEvent(d, w, False, NoEventMask, &close);
    XFlush(d);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    FILE *f = fopen(pbm, "rb");
    assert(f);
    char header[32];
    assert(fgets(header, sizeof(header), f) && !strcmp(header, "P4\n"));
    assert(fgets(header, sizeof(header), f) && !strcmp(header, "120 64\n"));
    uint8_t actual[960];
    assert(fread(actual, 1, sizeof(actual), f) == sizeof(actual));
    fclose(f);
    /* Compare the fixed BASIC header, ignoring the blinking cursor below. */
    mk90      m;
    struct tm date = { .tm_year = 126, .tm_mon = 8, .tm_mday = 30 };
    char      error[4352];
    mk90_init(&m, 16384, &date);
    assert(mk90_load(&m, MK90_ASSET_DIR, NULL, error, sizeof(error)));
    for (unsigned i = 0; i < 3000; i++)
        mk90_run_us(&m, 1000);
    mk90_key(&m, 56, false);
    for (unsigned i = 0; i < 150; i++)
        mk90_run_us(&m, 1000);
    mk90_key(&m, 0, false);
    for (unsigned i = 0; i < 2000; i++)
        mk90_run_us(&m, 1000);
    uint8_t expected[120 * 64];
    mk90_lcd(&m, expected);
    for (unsigned i = 0; i < 120 * 8; i++)
        assert(((actual[i / 8] >> (7 - i % 8)) & 1) == expected[i]);
    mk90_destroy(&m);
    XCloseDisplay(d);
    printf("X11 keyboard, mouse at 2x, reset, overlay, focus loss and close passed.\nScreenshot: %s\n", ppm);
    return 0;
}
