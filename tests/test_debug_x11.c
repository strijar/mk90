/* Explicit integration test: opens its own emulator window on DISPLAY.
 * Not in the default CTest suite, which remains usable without an X server.
 */
#define _POSIX_C_SOURCE 200809L
#include "mk90.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t emulator_pid;

static void delay(unsigned ms) {
    struct timespec t = { ms / 1000, (long) (ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}
static Window find_window(Display *d, Window root, pid_t pid, const char *title) {
    Atom           type;
    int            format;
    unsigned long  count, remaining;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, root, XInternAtom(d, "_NET_WM_PID", False), 0, 1, False, XA_CARDINAL, &type, &format, &count, &remaining, &data) == Success && data) {
        bool match = format == 32 && count == 1 && *(unsigned long *) data == (unsigned long) pid;
        XFree(data);
        if (match) {
            char *name = NULL;
            XFetchName(d, root, &name);
            bool found = name && !strcmp(name, title);
            if (name)
                XFree(name);
            if (found)
                return root;
        }
    }
    Window   actual, parent, *children = NULL, found = 0;
    unsigned n;
    if (XQueryTree(d, root, &actual, &parent, &children, &n)) {
        for (unsigned i = 0; i < n && !found; i++)
            found = find_window(d, children[i], pid, title);
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
    if (XkbKeycodeToKeysym(d, e.xkey.keycode, 0, 0) != sym && XkbKeycodeToKeysym(d, e.xkey.keycode, 0, 1) == sym)
        e.xkey.state = ShiftMask;
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

static void tap(Display *d, Window w, KeySym sym) {
    send_key(d, w, sym, true);
    delay(25);
    send_key(d, w, sym, false);
    delay(25);
}
static void text(Display *d, Window w, const char *s) {
    for (; *s; s++) {
        send_key(d, w, (unsigned char) *s, true);
        send_key(d, w, (unsigned char) *s, false);
    }
    XFlush(d);
    delay(150);
}
static void field(Display *d, Window w, int x, int y, const char *s) {
    click(d, w, x, y);
    send_key(d, w, XK_Control_L, true);
    tap(d, w, XK_a);
    send_key(d, w, XK_Control_L, false);
    text(d, w, s);
}
static void close_window(Display *d, Window w) {
    XEvent event = { 0 };
    event.xclient.type = ClientMessage;
    event.xclient.window = w;
    event.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long) XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSendEvent(d, w, False, NoEventMask, &event);
    XFlush(d);
    delay(150);
}
static void exact_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        perror(path);
    if (!f) {
        kill(emulator_pid, SIGTERM);
        waitpid(emulator_pid, NULL, 0);
    }
    assert(f);
    for (size_t i = 0; i < size; i++)
        assert(fgetc(f) == data[i]);
    assert(fgetc(f) == EOF);
    fclose(f);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fputs("No X11 display\n", stderr);
        return 1;
    }
    char dir[] = "/tmp/mk90-debug-x11-XXXXXX";
    assert(mkdtemp(dir));
    char input[128], output[128], other[128], snapshot[128], lcd[128];
    snprintf(input, sizeof(input), "%s/input.bin", dir);
    snprintf(output, sizeof(output), "%s/output.bin", dir);
    snprintf(other, sizeof(other), "%s/other.bin", dir);
    snprintf(snapshot, sizeof(snapshot), "%s/debugger.ppm", dir);
    snprintf(lcd, sizeof(lcd), "%s/lcd.pbm", dir);
    uint8_t data[8192];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t) (i * 13);
    FILE *f = fopen(input, "wb");
    assert(f && fwrite(data, 1, sizeof(data), f) == sizeof(data) && !fclose(f));
    f = fopen(other, "wb");
    assert(f && fputc(0xa5, f) != EOF && !fclose(f));
    pid_t child = fork();
    emulator_pid = child;
    assert(child >= 0);
    if (!child) {
        execl(argv[1], argv[1], "--debugger", "--seconds", "30", "--no-save", "--debug-screenshot", snapshot, "--lcd", lcd, (char *) NULL);
        _exit(127);
    }
    Window main = 0, debug = 0;
    for (unsigned i = 0; i < 200 && (!main || !debug); i++) {
        delay(20);
        main = find_window(d, DefaultRootWindow(d), child, "MK-90");
        debug = find_window(d, DefaultRootWindow(d), child, "MK-90 Debugger");
    }
    if (!main || !debug) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        fputs("Both windows must open\n", stderr);
        return 1;
    }
    XResizeWindow(d, debug, 1120, 760);
    XFlush(d);
    delay(250);
    tap(d, debug, XK_F6); /* step from reset vector */
    field(d, debug, 300, 650, input);
    click(d, debug, 950, 650); /* load SMP0 */
    field(d, debug, 300, 650, output);
    click(d, debug, 1050, 650); /* export */
    exact_file(output, data, sizeof(data));
    /* Dirty-card replacement and existing-file overwrite must be cancellable. */
    field(d, debug, 300, 650, other);
    click(d, debug, 950, 650);
    tap(d, debug, XK_Escape);
    field(d, debug, 300, 650, output);
    click(d, debug, 1050, 650);
    tap(d, debug, XK_Escape);
    exact_file(output, data, sizeof(data));
    /* Confirm replacement, then confirm overwrite. */
    field(d, debug, 300, 650, other);
    click(d, debug, 950, 650);
    click(d, debug, 340, 413);
    field(d, debug, 300, 650, output);
    click(d, debug, 1050, 650);
    click(d, debug, 340, 413);
    const uint8_t one[] = { 0xa5 };
    exact_file(output, one, sizeof(one));
    /* Resume execution, then exercise independent window lifetimes. */
    tap(d, debug, XK_F5);
    delay(1800);
    close_window(d, debug);
    delay(150);
    assert(find_window(d, DefaultRootWindow(d), child, "MK-90") != 0);
    assert(find_window(d, DefaultRootWindow(d), child, "MK-90 Debugger") == 0);
    tap(d, main, XK_F3);
    delay(300);
    debug = find_window(d, DefaultRootWindow(d), child, "MK-90 Debugger");
    assert(debug);
    close_window(d, main); /* Both windows must be disposed, even while paused. */
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    XCloseDisplay(d);
    printf("Debugger X11: two windows, step, editable path, SMP load/export, confirmation/cancel, reopen and shutdown passed.\nArtifacts: %s\n", dir);
    return 0;
}
