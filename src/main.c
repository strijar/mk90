#define _POSIX_C_SOURCE 200809L

#include "mk90.h"
#ifdef MK90_GUI
#include "ui.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program) {
    printf("Usage: %s [options]\n"
           "  --assets DIR      ROM and initial card images (default: bundled assets)\n"
           "  --state-dir DIR   Writable card images (default: mk90/state)\n"
           "  --no-save         Use original cards without reading/writing state\n"
           "  --ram KB          RAM size: 16..32 (default: 16)\n"
           "  --speed N         Instruction budget per second (default: 100000)\n"
           "  --headless        Run core without a window (default: 5 seconds)\n"
           "  --seconds N       Emulated seconds in headless mode; time limit in GUI\n"
           "  --lcd FILE        Save LCD as a binary PBM on exit\n"
           "  --ui-smoke        Render LVGL offscreen without an X server\n"
           "  --screenshot FILE Save full LVGL interface as PPM on exit\n"
           "  --help            Show this help\n",
           program);
}

static bool number(const char *s, unsigned min, unsigned max, unsigned *result) {
    char *end;
    errno = 0;

    unsigned long n = strtoul(s, &end, 10);

    if (errno || !*s || *end || n < min || n > max)
        return false;

    *result = (unsigned) n;
    return true;
}

int main(int argc, char **argv) {
    const char *assets = MK90_ASSET_DIR, *state = MK90_STATE_DIR;
    const char *lcd = NULL, *screenshot = NULL;
    unsigned    ram = 16, speed = 100000, seconds = 0;
    bool        headless = false, smoke = false, no_save = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--help")) {
            usage(argv[0]);
            return 0;
        }

        if (!strcmp(arg, "--headless")) {
            headless = true;
            continue;
        }

        if (!strcmp(arg, "--ui-smoke")) {
            smoke = true;
            continue;
        }

        if (!strcmp(arg, "--no-save")) {
            no_save = true;
            continue;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value: %s\n", arg);
            return 2;
        }

        const char *value = argv[++i];
        bool        ok = true;

        if (!strcmp(arg, "--assets"))
            assets = value;
        else if (!strcmp(arg, "--state-dir"))
            state = value;
        else if (!strcmp(arg, "--lcd"))
            lcd = value;
        else if (!strcmp(arg, "--screenshot"))
            screenshot = value;
        else if (!strcmp(arg, "--ram"))
            ok = number(value, 16, 32, &ram);
        else if (!strcmp(arg, "--speed"))
            ok = number(value, 1000, 10000000, &speed);
        else if (!strcmp(arg, "--seconds"))
            ok = number(value, 1, 86400, &seconds);
        else
            ok = false;

        if (!ok) {
            fprintf(stderr, "Invalid option/value: %s %s\n", arg, value);
            return 2;
        }
    }

    if (headless && (smoke || screenshot)) {
        fprintf(stderr, "Use --ui-smoke for GUI screenshots, --headless --lcd for LCD dumps.\n");
        return 2;
    }

    if (no_save)
        state = NULL;

    time_t    now = time(NULL);
    struct tm date;

    if (!localtime_r(&now, &date)) {
        fprintf(stderr, "Cannot read local time\n");
        return 1;
    }

    mk90 *m = malloc(sizeof(*m));

    if (!m) {
        perror("malloc");
        return 1;
    }

    mk90_init(m, ram * 1024, &date);
    m->speed = speed;

    char error[4352];

    if (!mk90_load(m, assets, state, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        mk90_destroy(m);
        free(m);
        return 1;
    }

    int result = 0;

    if (headless) {
        if (!seconds)
            seconds = 5;

        for (uint64_t i = 0; i < (uint64_t) seconds * 1000; i++)
            mk90_run_us(m, 1000);
        printf("PC=%04x PSW=%04x LCD=%04x instructions=%llu\n", m->r[7], m->psw, m->lcd[0], (unsigned long long) m->instructions);
    } else {
#ifdef MK90_GUI
        result = mk90_ui(m, seconds, smoke, screenshot);
#else
        (void) smoke;
        fprintf(stderr, "Built without GUI; run with --headless or configure MK90_GUI=ON.\n");
        result = 1;
#endif
    }

    if (lcd && !mk90_write_pbm(m, lcd)) {
        perror(lcd);
        result = 1;
    }

    if (!mk90_save(m, state, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        result = 1;
    }

    mk90_destroy(m);
    free(m);

    return result;
}
