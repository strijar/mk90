#ifndef MK90_CORE_H
#define MK90_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { MK90_N = 8,
       MK90_Z = 4,
       MK90_V = 2,
       MK90_C = 1,
       MK90_T = 16,
       MK90_I = 128,
       MK90_H = 256 };

enum { MK90_ROM_SIZE = 0xbf00,
       MK90_RAM_MAX = 0x8000,
       MK90_LCD_W = 120,
       MK90_LCD_H = 64 };

typedef struct {
    uint8_t *data;
    size_t   size;
    uint32_t position, mask;
    uint8_t  command;
    bool     dirty;
} mk90_smp;

typedef struct {
    uint16_t r[8], psw, opcode;
    uint8_t  ram[MK90_RAM_MAX], rom[MK90_ROM_SIZE];
    unsigned ram_size, speed;
    bool     wait, reset_pending, fault, halt_irq, evnt, virq[3];
    uint16_t sys1, sys2, lcd[2], io[4], shift, requests;
    bool     selected;
    uint16_t io_word, lcd_word, rtc_word;
    int      pending, pending_index, rtc_index;
    uint8_t  rtc[64], mouse_key, host_key;
    mk90_smp smp[2];
    uint64_t instructions, budget, time_fraction, rtc_us, second_us;
    unsigned last_period;
} mk90;

void mk90_init(mk90 *m, unsigned ram_size, const struct tm *date);
void mk90_reset(mk90 *m, const struct tm *date);
void mk90_destroy(mk90 *m);

/* ROMs/assets are read-only. Modified cards are saved separately by the host. */

bool     mk90_load(mk90 *m, const char *asset_dir, const char *state_dir, char *error, size_t error_size);
bool     mk90_save(mk90 *m, const char *state_dir, char *error, size_t error_size);
unsigned mk90_step(mk90 *m);
void     mk90_run_us(mk90 *m, unsigned us);
void     mk90_key(mk90 *m, unsigned key, bool mouse);
void     mk90_lcd(const mk90 *m, uint8_t pixels[MK90_LCD_W * MK90_LCD_H]);
bool     mk90_write_pbm(const mk90 *m, const char *path);
uint16_t mk90_read(mk90 *m, uint16_t address, bool byte);
void     mk90_write(mk90 *m, uint16_t address, uint16_t value, bool byte);
void     mk90_bus_finish(mk90 *m);
void     mk90_rtc_second(mk90 *m);
void     mk90_advance_clock(mk90 *m, unsigned us);

#endif
