#define _POSIX_C_SOURCE 200809L

#include "mk90.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool path_join(char *path, size_t size, const char *dir, const char *name) {
    int n = snprintf(path, size, "%s/%s", dir, name);

    if (n < 0 || (size_t) n >= size) {
        errno = ENAMETOOLONG;
        return false;
    }

    return true;
}

static bool failure(char *error, size_t size, const char *path) {
    snprintf(error, size, "%s: %s", path, strerror(errno));

    return false;
}

static bool load_rom(uint8_t *data, size_t capacity, const char *path, bool optional, char *error, size_t size) {
    FILE *f = fopen(path, "rb");

    if (!f)
        return optional && errno == ENOENT ? true : failure(error, size, path);

    size_t n = fread(data, 1, capacity, f);
    bool   ok = !ferror(f) && n > 0;

    if (fclose(f) != 0)
        ok = false;

    if (!ok) {
        errno = EIO;
        return failure(error, size, path);
    }

    return true;
}

bool mk90_load(mk90 *m, const char *assets, const char *state, char *error, size_t size) {
    char path[4096];

    if (!path_join(path, sizeof(path), assets, "rom.bin"))
        return failure(error, size, assets);

    if (!load_rom(m->rom + 0x4000, MK90_ROM_SIZE - 0x4000, path, false, error, size))
        return false;

    if (!path_join(path, sizeof(path), assets, "romt.bin"))
        return failure(error, size, assets);

    if (!load_rom(m->rom, 0x4000, path, true, error, size))
        return false;

    for (unsigned i = 0; i < 2; i++) {
        char name[16];

        snprintf(name, sizeof(name), "smp%u.bin", i);

        FILE *f = NULL;

        if (state) {
            if (!path_join(path, sizeof(path), state, name))
                return failure(error, size, state);

            f = fopen(path, "rb");

            if (!f && errno != ENOENT)
                return failure(error, size, path);
        }

        if (!f) {
            if (!path_join(path, sizeof(path), assets, name))
                return failure(error, size, assets);
            f = fopen(path, "rb");
        }

        if (!f) {
            if (errno == ENOENT)
                continue;
            return failure(error, size, path);
        }

        bool ok = fseek(f, 0, SEEK_END) == 0;
        long length = ok ? ftell(f) : -1;

        if (length <= 0 || length > 0x1000000 || fseek(f, 0, SEEK_SET)) {
            fclose(f);
            errno = EINVAL;
            return failure(error, size, path);
        }

        uint8_t *data = malloc((size_t) length);

        if (!data) {
            fclose(f);
            return failure(error, size, path);
        }

        ok = fread(data, 1, (size_t) length, f) == (size_t) length;

        if (fclose(f) != 0)
            ok = false;
        if (!ok) {
            free(data);
            errno = EIO;
            return failure(error, size, path);
        }

        free(m->smp[i].data);
        m->smp[i] = (mk90_smp) { .data = data, .size = (size_t) length, .mask = length < 0x10000 ? 0xffff : 0xffffff };
    }

    return true;
}

/* Write to a sibling temporary file then rename, never overwrite the assets. */
bool mk90_save(mk90 *m, const char *state, char *error, size_t size) {
    if (!state)
        return true;

    bool dirty = m->smp[0].dirty || m->smp[1].dirty;

    if (!dirty)
        return true;

    if (mkdir(state, 0700) && errno != EEXIST)
        return failure(error, size, state);

    for (unsigned i = 0; i < 2; i++) {
        mk90_smp *s = &m->smp[i];

        if (!s->dirty)
            continue;

        char path[4096], temporary[4096], name[32];

        snprintf(name, sizeof(name), "smp%u.bin", i);

        if (!path_join(path, sizeof(path), state, name))
            return failure(error, size, state);

        snprintf(name, sizeof(name), "smp%u.bin.tmp", i);

        if (!path_join(temporary, sizeof(temporary), state, name))
            return failure(error, size, state);

        FILE *f = fopen(temporary, "wb");

        if (!f)
            return failure(error, size, temporary);

        bool ok = fwrite(s->data, 1, s->size, f) == s->size;

        if (fclose(f) != 0)
            ok = false;

        if (!ok) {
            errno = EIO;
            return failure(error, size, temporary);
        }

        if (rename(temporary, path))
            return failure(error, size, path);

        s->dirty = false;
    }
    return true;
}

bool mk90_write_pbm(const mk90 *m, const char *path) {
    uint8_t pixels[MK90_LCD_W * MK90_LCD_H];

    mk90_lcd(m, pixels);

    FILE *f = fopen(path, "wb");

    if (!f)
        return false;

    bool ok = fprintf(f, "P4\n120 64\n") > 0;

    for (unsigned i = 0; i < sizeof(pixels); i += 8) {
        unsigned b = 0;

        for (unsigned j = 0; j < 8; j++)
            b = (b << 1) | pixels[i + j];

        if (fputc((int) b, f) == EOF)
            ok = false;
    }

    if (fclose(f))
        ok = false;

    return ok;
}
