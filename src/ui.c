#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "artwork.h"
#include "x11_host.h"
#include <X11/keysym.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct {
    mk90          *machine;
    lv_obj_t      *root, *lcd, *overlay[8], *down[63];
    uint8_t        lcd_pixels[120 * 64 * 4], mono[120 * 64];
    lv_image_dsc_t lcd_image;
    bool           running, overlay_visible, paused;
    unsigned       mouse_key, host_key;
} ui;

static volatile sig_atomic_t interrupted;

static void stop_signal(int sig) {
    (void) sig;
    interrupted = 1;
}

static uint64_t microseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    return (uint64_t) t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static uint32_t tick(void) {
    return (uint32_t) (microseconds() / 1000);
}

static void refresh_keys(void) {
    for (unsigned i = 1; i <= 63; i++) {
        bool pressed = i == ui.mouse_key || i == ui.host_key;
        lv_obj_set_hidden(ui.down[i - 1], !pressed);
    }
}

static void reset(void) {
    time_t    now = time(NULL);

    struct tm date;

    if (localtime_r(&now, &date))
        mk90_reset(ui.machine, &date);

    ui.mouse_key = ui.host_key = 0;
    refresh_keys();
}

static void toggle_overlay(void) {
    ui.overlay_visible = !ui.overlay_visible;

    for (unsigned i = 0; i < 8; i++) {
        lv_obj_set_hidden(ui.overlay[i], !ui.overlay_visible);
    }
}

static void mouse_event(lv_event_t *e) {
    unsigned        key = (unsigned) (uintptr_t) lv_event_get_user_data(e);

    lv_event_code_t code = lv_event_get_code(e);

    if (!ui.running || (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED &&
                        code != LV_EVENT_PRESS_LOST && code != LV_EVENT_CLICKED))
        return;

    if (code == LV_EVENT_PRESSED) {
        ui.mouse_key = key;
        mk90_key(ui.machine, key, true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (ui.mouse_key == key) {
            ui.mouse_key = 0;
            mk90_key(ui.machine, 0, true);
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (key == 1)
            ui.running = false;

        if (key == 2)
            reset();
    }

    refresh_keys();
}

static unsigned mapped_key(unsigned long symbol) {
    switch (symbol) {
        case XK_Up:
            return 50;

        case XK_Left:
            return 51;

        case XK_Right:
            return 54;

        case XK_BackSpace:
            return 55;

        case XK_Return:
        case XK_KP_Enter:
            return 56;

        case XK_Down:
            return 58;

        case XK_Page_Down:
            return 59;

        case XK_space:
            return 60;

        case XK_Page_Up:
            return 61;

        case XK_F6:
            return 49;

        case XK_F7:
            return 57;

        case XK_F8:
            return 62;

        case XK_F9:
            return 63;
    }

    /* Keyboard mapping of the original Windows emulator. */

    const char *letters = "12345:;67890/-ABWGDEVZIJKLMNOPRSTUFHC^[]XY_\\@Qaaa,.";

    if (symbol >= 32 && symbol < 127) {
        const char *p = strchr(letters, toupper((unsigned char) symbol));

        if (p)
            return (unsigned) (p - letters) + 3;
    }

    return 0;
}

static void host_key(unsigned long symbol, bool down) {
    if (down) {
        if (symbol == XK_F2) {
            toggle_overlay();
            return;
        }

        if (symbol == XK_F5) {
            reset();
            return;
        }

        if (symbol == XK_Pause) {
            ui.paused = !ui.paused;
            return;
        }
    }

    unsigned key = mapped_key(symbol);

    if (!key)
        return;

    if (down) {
        ui.host_key = key;
        mk90_key(ui.machine, key, false);
    } else if (ui.host_key == key) {
        ui.host_key = 0;
        mk90_key(ui.machine, 0, false);
    }

    refresh_keys();
}

static lv_obj_t *picture(lv_obj_t *parent, const lv_image_dsc_t *image, int x, int y) {
    lv_obj_t *obj = lv_image_create(parent);

    lv_image_set_src(obj, image);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_clickable(obj, false);

    return obj;
}

static void resize(lv_event_t *e) {
    lv_display_t *display = lv_event_get_user_data(e);
    int           w = lv_display_get_horizontal_resolution(display), h = lv_display_get_vertical_resolution(display);
    int           scale = w * 256 / 790;

    if (h * 256 / 312 < scale)
        scale = h * 256 / 312;

    if (scale < 1)
        scale = 1;

    lv_obj_set_style_transform_pivot_x(ui.root, 0, 0);
    lv_obj_set_style_transform_pivot_y(ui.root, 0, 0);
    lv_obj_set_style_transform_scale(ui.root, scale, 0);
    lv_obj_set_pos(ui.root, (w - 790 * scale / 256) / 2, (h - 312 * scale / 256) / 2);
}

static void create_ui(lv_display_t *display) {
    lv_obj_t *screen = lv_screen_active();

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x202020), 0);
    lv_obj_set_scrollable(screen, false);
    ui.root = lv_obj_create(screen);
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_size(ui.root, 790, 312);
    lv_obj_set_scrollable(ui.root, false);

    picture(ui.root, &mk90_face, 0, 0);

    ui.lcd_image = (lv_image_dsc_t) {
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 120, .h = 64, .stride = 480 },
        .data_size = sizeof(ui.lcd_pixels),
        .data = ui.lcd_pixels
    };

    memset(ui.lcd_pixels, 255, sizeof(ui.lcd_pixels));

    ui.lcd = picture(ui.root, &ui.lcd_image, 62, 55);
    lv_image_set_pivot(ui.lcd, 0, 0);
    lv_image_set_scale(ui.lcd, 768);
    lv_image_set_antialias(ui.lcd, false);

    static const int rows[8] = { 25, 58, 93, 124, 155, 186, 217, 248 };

    for (unsigned i = 0; i < 8; i++) {
        ui.overlay[i] = picture(ui.root, mk90_overlays[i], 486, rows[i]);
        lv_obj_set_hidden(ui.overlay[i], true);
    }

    for (unsigned i = 0; i < 63; i++) {
        const mk90_key_art *a = &mk90_keys[i];
        lv_obj_t           *button = lv_obj_create(ui.root);

        lv_obj_remove_style_all(button);
        lv_obj_set_pos(button, a->x, a->y);
        lv_obj_set_size(button, a->w, a->h);
        lv_obj_set_scrollable(button, false);
        lv_obj_set_clickable(button, true);
        lv_obj_add_event_cb(button, mouse_event, LV_EVENT_ALL, (void *) (uintptr_t) (i + 1));

        ui.down[i] = picture(button, a->down, 0, 0);
        lv_obj_set_hidden(ui.down[i], true);
    }

    lv_display_add_event_cb(display, resize, LV_EVENT_RESOLUTION_CHANGED, display);
}

static void update_lcd(void) {
    uint8_t pixels[120 * 64];

    mk90_lcd(ui.machine, pixels);

    if (!memcmp(pixels, ui.mono, sizeof(pixels)))
        return;

    memcpy(ui.mono, pixels, sizeof(pixels));

    for (unsigned i = 0; i < sizeof(pixels); i++) {
        memset(ui.lcd_pixels + i * 4, pixels[i] ? 0 : 255, 3);
        ui.lcd_pixels[i * 4 + 3] = 255;
    }

    /* Raw images, caches disabled in lv_conf.h: invalidation reads the buffer. */

    lv_obj_invalidate(ui.lcd);
}

static bool snapshot(const char *path) {
    lv_obj_update_layout(ui.root);

    lv_draw_buf_t *buf = lv_snapshot_take(ui.root, LV_COLOR_FORMAT_ARGB8888);

    if (!buf)
        return false;

    FILE *f = fopen(path, "wb");
    bool  ok = f != NULL;

    if (f) {
        ok = fprintf(f, "P6\n%u %u\n255\n", buf->header.w, buf->header.h) > 0;

        for (unsigned y = 0; y < buf->header.h; y++)
            for (unsigned x = 0; x < buf->header.w; x++) {
                const uint8_t *p = buf->data + y * buf->header.stride + x * 4;
                uint8_t        rgb[3] = { p[2], p[1], p[0] };

                if (fwrite(rgb, 1, 3, f) != 3)
                    ok = false;
            }

        if (fclose(f))
            ok = false;
    }

    lv_draw_buf_destroy(buf);

    return ok;
}

static void discard_flush(lv_display_t *d, const lv_area_t *area, uint8_t *pixels) {
    (void) area;
    (void) pixels;
    lv_display_flush_ready(d);
}

int mk90_ui(mk90 *m, unsigned seconds, bool smoke, const char *screenshot) {
    memset(&ui, 0, sizeof(ui));

    ui.machine = m;
    ui.running = true;
    interrupted = 0;

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);

    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t  *display;

    static uint8_t offscreen[790 * 32 * 4];

    if (smoke) {
        display = lv_display_create(790, 312);
        lv_display_set_buffers(display, offscreen, NULL, sizeof(offscreen), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, discard_flush);

        if (!seconds)
            seconds = 2;
    } else
        display = mk90_x11_open(host_key);

    if (!display) {
        lv_deinit();
        return 1;
    }

    create_ui(display);
    uint64_t start = microseconds(), last = start, last_frame = 0, simulated = 0;

    while (ui.running && !interrupted) {
        if (!smoke && !mk90_x11_poll())
            break;

        uint64_t now = microseconds();
        unsigned elapsed = smoke ? 1000 : (unsigned) (now - last);

        /* Drop very long host stalls; never block input catching up indefinitely. */

        if (elapsed > 100000)
            elapsed = 100000;

        last = now;

        if (!ui.paused)
            mk90_run_us(m, elapsed);

        simulated += elapsed;

        if (smoke || now - last_frame >= 32000) {
            update_lcd();
            last_frame = now;
        }

        lv_timer_handler();

        if (seconds && (smoke ? simulated : now - start) >= (uint64_t) seconds * 1000000)
            break;

        if (!smoke) {
            struct timespec delay = { 0, 1000000 };
            nanosleep(&delay, NULL);
        }
    }

    update_lcd();

    int result = 0;

    if (screenshot && !snapshot(screenshot)) {
        fprintf(stderr, "Cannot save screenshot: %s\n", screenshot);
        result = 1;
    }

    ui.running = false;

    if (smoke)
        lv_display_delete(display);
    else
        mk90_x11_close();

    lv_deinit();

    return result;
}
