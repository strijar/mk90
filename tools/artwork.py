#!/usr/bin/env python3
"""Convert the original uncompressed 4-bit BMP artwork to LVGL C resources.

Uses only Python's standard library; no image codecs are needed at runtime.
"""
import pathlib
import struct
import sys


def bmp(path):
    data = path.read_bytes()
    offset = struct.unpack_from('<I', data, 10)[0]
    header, width, height, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    if data[:2] != b'BM' or header != 40 or planes != 1 or bits != 4 or compression != 0:
        raise ValueError(f'{path}: expected an uncompressed 4-bit Windows BMP')
    palette = [tuple(data[54 + i * 4:57 + i * 4]) + (255,) for i in range(16)]
    stride = ((width * bits + 31) // 32) * 4
    rows = []
    for y in range(abs(height)):
        row = offset + (abs(height) - y - 1 if height > 0 else y) * stride
        rows.append([palette[(data[row + x // 2] >> (0 if x % 2 else 4)) & 15] for x in range(width)])
    return rows


def image(out, name, rows):
    h, w = len(rows), len(rows[0])
    out.write(f'static const uint8_t {name}_data[] = {{\n')
    for row in rows:
        out.write(','.join(str(c) for pixel in row for c in pixel) + ',\n')
    out.write('};\n')
    out.write(f'const lv_image_dsc_t {name} = {{ .header = {{.magic=LV_IMAGE_HEADER_MAGIC, '
              f'.cf=LV_COLOR_FORMAT_ARGB8888, .w={w}, .h={h}, .stride={w*4}}}, '
              f'.data_size=sizeof({name}_data), .data={name}_data }};\n')


def main():
    assets, destination = map(pathlib.Path, sys.argv[1:])
    face, keys, overlay = (bmp(assets / n) for n in ('face.bmp', 'keys.bmp', 'overlay.bmp'))
    # Geometry from keyboard.pas, including power/reset.
    blocks = [(491,37,27,18,35,33,1,2,54,18), (526,35,27,22,35,33,7,14,0,18),
              (491,103,27,18,35,31,8,40,54,18), (491,258,27,18,35,31,3,3,54,18),
              (596,258,62,18,70,31,1,1,0,0), (666,258,27,18,35,31,3,3,54,18)]
    with destination.open('w') as out:
        out.write('/* Generated from bundled BMPs; do not edit. */\n#include <lvgl.h>\n#include "artwork.h"\n')
        image(out, 'mk90_face', face)
        for i in range(8):
            image(out, f'mk90_overlay_{i}', overlay[i*9:(i+1)*9])
        geometry = []
        for left, top, w, h, sx, sy, cols, count, ox, oy in blocks:
            for k in range(count):
                x, y = left + (k % cols) * sx, top + (k // cols) * sy
                original = [row[x:x+w] for row in face[y:y+h]]
                down = [row[:] for row in original]
                for yy in range(h-8):
                    for xx in range(w-8):
                        down[yy+5][xx+5] = original[yy+4][xx+4]
                for yy in range(h):
                    for xx in range(w):
                        pixel = keys[oy+yy][ox+xx]
                        if pixel[:3] != (0,255,0):
                            down[yy][xx] = pixel
                image(out, f'mk90_down_{len(geometry)+1}', down)
                geometry.append((x, y, w, h))
        out.write('const mk90_key_art mk90_keys[63] = {\n')
        for i, (x,y,w,h) in enumerate(geometry):
            out.write(f'{{{x},{y},{w},{h},&mk90_down_{i+1}}},\n')
        out.write('};\nconst lv_image_dsc_t *const mk90_overlays[8] = {\n')
        out.write(','.join(f'&mk90_overlay_{i}' for i in range(8)))
        out.write('};\n')


if __name__ == '__main__':
    main()
