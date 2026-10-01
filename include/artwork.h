#ifndef MK90_ARTWORK_H
#define MK90_ARTWORK_H

#include <lvgl.h>

typedef struct {
    int                   x, y, w, h;
    const lv_image_dsc_t *down;
} mk90_key_art;

extern const lv_image_dsc_t        mk90_face;
extern const mk90_key_art          mk90_keys[63];
extern const lv_image_dsc_t *const mk90_overlays[8];

#endif
