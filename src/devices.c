/* Memory map, KA1835 controllers, serial cards and KA512VI1 RTC.
 * Ported from emul_src/{def,syscon,iosystem,smp,rtc,lcd,keyboard}.pas.
 */
#include "mk90.h"
#include "debugger.h"
#include <stdlib.h>
#include <string.h>

enum { P_NONE,
       P_IO,
       P_LCD,
       P_RTC_VRT,
       P_RTC_CLEAR
};

static const uint16_t split[8] = { 0xe000, 0x4000, 0x8000, 0x2000, 0, 0, 0, 0 };
static const unsigned periods[16] = { 0, 3906, 7812, 122, 244, 488, 977, 1953, 3906, 7812, 15625, 31250, 62500, 125000, 250000, 500000 };

static const uint8_t keytab[64] = {
    0,
    0,
    0,
    0x23,
    0x43,
    0x63,
    0x83,
    0xa3,
    0xc3,
    0xe3,
    0x27,
    0x47,
    0x67,
    0x87,
    0xa7,
    0xc7,
    0xe7,
    0x0b,
    0x2b,
    0x4b,
    0x6b,
    0x8b,
    0xab,
    0xcb,
    0xeb,
    0x0f,
    0x2f,
    0x4f,
    0x6f,
    0x8f,
    0xaf,
    0xcf,
    0xef,
    0x13,
    0x33,
    0x53,
    0x73,
    0x93,
    0xb3,
    0xd3,
    0xf3,
    0x17,
    0x37,
    0x57,
    0x77,
    0x97,
    0xb7,
    0xd7,
    0xf7,
    0x1b,
    0x3b,
    0x5b,
    0x7b,
    0x9b,
    0xbb,
    0xdb,
    0xfb,
    0x1f,
    0x3f,
    0x5f,
    0x7f,
    0xbf,
    0xdf,
    0xff
};

void mk90_key(mk90 *m, unsigned key, bool mouse) {
    if (key > 63)
        return;

    if (mouse)
        m->mouse_key = key;
    else
        m->host_key = key;

    if (key > 2) {
        m->requests &= ~4;

        if (!(m->io[2] & 0x10))
            m->virq[2] = true;
    }
}

static uint8_t smp_data(mk90_smp *s, uint8_t data) {
    uint8_t result = 255;

    if (!s->data)
        return result;

    switch (s->command & 0xf0) {
        case 0:
            return 0;

        case 0xa0:
            s->position = ((s->position << 8) | data) & s->mask;
            break;

        case 0x10:
        case 0xd0:
            if (s->position < s->size)
                result = s->data[s->position];
            s->position = (s->position + ((s->command & 0x80) ? 1u : UINT32_MAX)) & s->mask;
            break;

        case 0x20:
        case 0xc0:
        case 0xe0:
            /* Same threshold as the original: >=64 KiB is a ROM card. */
            if (s->position < s->size && s->size < 0x10000) {
                s->dirty |= s->data[s->position] != data;
                s->data[s->position] = data;
            }
            s->position = (s->position + ((s->command & 0x20) ? UINT32_MAX : 1u)) & s->mask;
            break;
    }
    return result;
}

static void transfer(mk90 *m, bool command, bool allow_write) {
    unsigned device = m->io[2] & 15;

    if (!(m->io[2] & 0x20))
        m->virq[1] = true;

    if (device == 2)
        m->shift = keytab[m->mouse_key > 2 ? m->mouse_key : m->host_key];
    else if (device < 2) {
        if (command) {
            m->smp[device].command = 0;
            m->shift = 0;
        } else
            m->shift = smp_data(&m->smp[device], 0);
    } else if (allow_write && (device == 8 || device == 9)) {
        if (command)
            m->smp[device & 1].command = m->shift;
        else
            (void) smp_data(&m->smp[device & 1], m->shift);
    }
}

void mk90_bus_finish(mk90 *m) {
    int p = m->pending, i = m->pending_index;

    m->pending = P_NONE;

    if (p == P_LCD)
        m->lcd[i] = m->lcd_word;
    else if (p == P_RTC_VRT)
        m->rtc[13] = 0x80;
    else if (p == P_RTC_CLEAR)
        m->rtc[12] = 0;
    else if (p == P_IO) {
        m->io[i] = m->io_word;

        if (i == 0 || i == 3) {
            m->shift = m->io_word;
            m->requests |= 1u << (m->io[2] & 7);
        }

        if (i == 3)
            m->selected = true;

        if (m->selected && i != 1)
            transfer(m, i == 3, i != 2);
    }
}

static uint16_t io_read(mk90 *m, unsigned index) {
    switch (index) {
        case 0:
            m->io_word = m->shift;
            m->shift = 0xffff;
            m->requests |= 1u << (m->io[2] & 7);

            if (m->selected)
                transfer(m, false, false);
            break;

        case 1:
            m->io_word = m->requests;
            break;

        case 2:
            m->io_word = (m->io[2] & 0x70) | 0xff84 | (m->selected ? 0 : 8);
            break;

        case 3:
            if (m->selected && !(m->io[2] & 0x20))
                m->virq[1] = true;

            m->io_word = m->shift;
            m->selected = false;
            break;
    }

    return m->io_word;
}

static void rtc_commit(mk90 *m) {
    if (m->rtc_index >= 0 && m->rtc_index < 64 && m->rtc_index != 12 && m->rtc_index != 13)
        m->rtc[m->rtc_index] = m->rtc_word >> 1;

    m->rtc_index = -1;
}

static bool io_area(uint16_t a) {
    return a >= 0xe800 && a < 0xec00;
}

bool mk90_peek(const mk90 *m, uint16_t a, bool byte, mk90_memory_view view, uint16_t *value) {
    *value = byte ? 255 : 65535;

    bool ram = view == MK90_VIEW_RAM;
    bool rom = view == MK90_VIEW_ROM;

    if (view == MK90_VIEW_CPU) {
        ram = a < split[(m->sys1 >> 11) & 3] || io_area(a);
        rom = !ram && !(m->sys2 & 0x2000) &&
              ((a >= split[(m->sys1 >> 11) & 7] && a < 0xe000) ||
               ((m->sys2 & 0x200) && a >= 0xe000 && a < 0xe800) ||
               (a >= 0xec00 && a < 0xfe00));
    }

    if (ram && a < m->ram_size) {
        if (!byte && a + 1u >= m->ram_size)
            return false;

        *value = m->ram[a] | (byte ? 0 : (uint16_t) m->ram[a + 1] << 8);
        return true;
    }

    if (rom && a >= 0x4000 && (unsigned) (a - 0x4000) + (byte ? 0 : 1) < MK90_ROM_SIZE) {
        *value = m->rom[a - 0x4000] | (byte ? 0 : (uint16_t) m->rom[a - 0x4000 + 1] << 8);
        return true;
    }

    if (view != MK90_VIEW_CPU || !ram)
        return false;

    uint16_t result;

    if ((a & 0xfff8) == 0xe810) {
        switch ((a >> 1) & 3) {
            case 0:
            case 3:
                result = m->shift;
                break;

            case 1:
                result = m->requests;
                break;

            default:
                result = (m->io[2] & 0x70) | 0xff84 | (m->selected ? 0 : 8);
                break;
        }
    } else if (a == 0xe81a)
        result = m->sys1;
    else if (a == 0xe81c)
        result = m->sys2;
    else if (a == 0xe818 || a == 0xe81e)
        result = 0xffff;
    else if ((a & 0xff00) == 0xea00) {
        unsigned index = (a >> 1) & 63;

        /* Show the value a read would see, without committing the pending write. */
        uint8_t data = m->rtc[index];

        if (m->rtc_index == (int) index && index != 12 && index != 13)
            data = m->rtc_word >> 1;

        result = (uint16_t) data << 1;
    } else
        return false;

    *value = byte ? result & 255 : result;
    return true;
}

uint16_t mk90_read(mk90 *m, uint16_t a, bool byte) {
    uint16_t value = 0xffff;

    if (a < split[(m->sys1 >> 11) & 3] || io_area(a)) {
        if (a < m->ram_size) {
            value = m->ram[a];

            if (!byte)
                value |= (uint16_t) (a + 1u < m->ram_size ? m->ram[a + 1] : 255) << 8;
        } else if ((a & 0xfff8) == 0xe810)
            value = io_read(m, (a >> 1) & 3);
        else if (a == 0xe81a)
            value = m->sys1;
        else if (a == 0xe81c)
            value = m->sys2;
        else if ((a & 0xff00) == 0xea00) {
            rtc_commit(m);
            unsigned index = (a >> 1) & 63;
            m->rtc_word = (uint16_t) m->rtc[index] << 1;

            if (index == 13)
                m->pending = P_RTC_VRT;
            else if (index == 12)
                m->pending = P_RTC_CLEAR;
            value = m->rtc_word;
        } else if (a != 0xe818 && a != 0xe81e && a >= 0xe800)
            m->fault = true;
    } else if (!(m->sys2 & 0x2000) &&
               ((a >= split[(m->sys1 >> 11) & 7] && a < 0xe000) ||
                ((m->sys2 & 0x200) && a >= 0xe000 && a < 0xe800) ||
                (a >= 0xec00 && a < 0xfe00))) {
        if (a >= 0x4000 && (unsigned) (a - 0x4000) < MK90_ROM_SIZE) {
            value = m->rom[a - 0x4000];
            if (!byte)
                value |= (uint16_t) m->rom[a - 0x4000 + 1] << 8;
        }
    } else if (a >= 0xe800)
        m->fault = true;

    return byte ? value & 255 : value;
}

static void latch(uint16_t *dest, uint16_t value, bool byte) {
    *dest = byte ? (*dest & 0xff00) | (value & 255) : value;
}

void mk90_write(mk90 *m, uint16_t a, uint16_t value, bool byte) {
    if (a < split[(m->sys1 >> 9) & 3] || io_area(a)) {
        if (a < m->ram_size) {
            m->ram[a] = value;

            if (!byte && a + 1u < m->ram_size)
                m->ram[a + 1] = value >> 8;
        } else if ((a & 0xfff8) == 0xe800) {
            latch(&m->lcd_word, value, byte);
            m->pending = P_LCD;
            m->pending_index = (a >> 1) & 1;
        } else if ((a & 0xfff8) == 0xe810) {
            latch(&m->io_word, value, byte);
            m->pending = P_IO;
            m->pending_index = (a >> 1) & 3;
        } else if (a == 0xe81a)
            latch(&m->sys1, value, byte);
        else if (a == 0xe81c)
            latch(&m->sys2, value, byte);
        else if ((a & 0xff00) == 0xea00) {
            rtc_commit(m);
            m->rtc_index = (a >> 1) & 63;
            latch(&m->rtc_word, value, byte);
        } else if (a != 0xe818 && a != 0xe81e && a >= 0xe800)
            m->fault = true;
    } else
        m->fault = true;
}

void mk90_reset(mk90 *m, const struct tm *date) {
    m->pending = 0;
    m->rtc_index = -1;
    m->wait = m->fault = m->halt_irq = m->evnt = false;
    memset(m->virq, 0, sizeof(m->virq));
    m->reset_pending = true;
    m->r[7] = 0xf600;
    m->psw = 0x00e0;
    m->sys1 = m->sys2 = 0;
    m->shift = m->requests = 0xffff;
    m->selected = false;
    m->mouse_key = m->host_key = 0;
    m->budget = m->time_fraction = m->rtc_us = m->second_us = 0;
    m->last_period = 0;

    for (unsigned i = 0; i < 2; i++) {
        m->smp[i].position = 0;
        m->smp[i].command = 0;
    }

    /* Public Pascal release initializes the clock, not battery RAM from disk. */

    m->rtc[13] = m->rtc[12] = 0;
    m->rtc[11] = 7;
    m->rtc[10] = 0x7f;
    m->rtc[1] = m->rtc[3] = m->rtc[5] = 0;
    m->rtc[0] = date->tm_sec;
    m->rtc[2] = date->tm_min;
    m->rtc[4] = date->tm_hour;
    m->rtc[6] = date->tm_wday ? date->tm_wday : 7;
    m->rtc[7] = date->tm_mday;
    m->rtc[8] = date->tm_mon + 1;
    m->rtc[9] = (date->tm_year + 1900) % 100;
}

void mk90_init(mk90 *m, unsigned ram_size, const struct tm *date) {
    memset(m, 0, sizeof(*m));
    memset(m->rom, 255, sizeof(m->rom));
    m->ram_size = ram_size < 0x4000 ? 0x4000 : ram_size > 0x8000 ? 0x8000
                                                                 : (ram_size + 15) & ~15u;
    m->speed = 100000;
    mk90_reset(m, date);
}

void mk90_destroy(mk90 *m) {
    for (unsigned i = 0; i < 2; i++) {
        free(m->smp[i].data);
        m->smp[i].data = NULL;
    }
}

static unsigned decode(uint8_t x, bool binary) {
    return binary ? x : (x >> 4) * 10 + (x & 15);
}
static uint8_t encode(unsigned x, bool binary) {
    return binary ? x : ((x / 10) << 4) | (x % 10);
}
static void timer_irq(mk90 *m) {
    if (!(m->io[2] & 0x40))
        m->virq[0] = true;
}

void mk90_rtc_second(mk90 *m) {
    rtc_commit(m);

    if (m->rtc[11] & 0x80) {
        m->rtc[12] &= ~0x10;
        return;
    }

    bool     binary = (m->rtc[11] & 4) != 0;
    unsigned sec = decode(m->rtc[0], binary), min = decode(m->rtc[2], binary);
    bool     mode24 = (m->rtc[11] & 2) != 0;
    unsigned hour = decode(m->rtc[4] & (mode24 ? 255 : 127), binary);

    if (!mode24)
        hour = hour % 12 + ((m->rtc[4] & 128) ? 12 : 0);
    if (++sec >= 60) {
        sec = 0;

        if (++min >= 60) {
            min = 0;

            if (++hour >= 24) {
                hour = 0;

                static const unsigned days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                unsigned              month = decode(m->rtc[8], binary);
                unsigned              year = decode(m->rtc[9], binary);
                unsigned              day = decode(m->rtc[7], binary) + 1;

                if (month < 1 || month > 12)
                    month = 1;

                unsigned limit = days[month - 1] + (month == 2 && year % 4 == 0);

                if (day > limit) {
                    day = 1;

                    if (++month > 12) {
                        month = 1;
                        year = (year + 1) % 100;
                    }
                }
                m->rtc[6] = encode(decode(m->rtc[6], binary) % 7 + 1, binary);
                m->rtc[7] = encode(day, binary);
                m->rtc[8] = encode(month, binary);
                m->rtc[9] = encode(year, binary);
            }
        }
    }

    m->rtc[0] = encode(sec, binary);
    m->rtc[2] = encode(min, binary);
    m->rtc[4] = mode24 ? encode(hour, binary) : encode(hour % 12 ? hour % 12 : 12, binary) | (hour >= 12 ? 128 : 0);

    bool alarm = true;

    for (unsigned i = 0; i < 6; i += 2)
        alarm &= (m->rtc[i + 1] == m->rtc[i]) || (m->rtc[i + 1] & 0xc0);

    if (alarm)
        m->rtc[12] |= 0x20;
    else
        m->rtc[12] &= ~0x20;

    m->rtc[12] |= 0x10;

    if (m->rtc[11] & m->rtc[12] & 0x70) {
        m->rtc[12] |= 0x80;
        timer_irq(m);
    } else
        m->rtc[12] &= ~0x80;
}

void mk90_advance_clock(mk90 *m, unsigned us) {
    unsigned period = periods[m->rtc[10] & 15];

    if (period != m->last_period) {
        m->rtc_us = 0;
        m->last_period = period;
    }

    if (period) {
        m->rtc_us += us;

        while (m->rtc_us >= period) {
            m->rtc_us -= period;

            if (m->rtc[11] & 8)
                m->evnt = true;
            m->rtc[12] |= 0x40;

            if (m->rtc[11] & 0x40) {
                m->rtc[12] |= 0x80;
                timer_irq(m);
            }
        }
    }

    m->second_us += us;

    while (m->second_us >= 1000000) {
        m->second_us -= 1000000;
        mk90_rtc_second(m);
    }
}

void mk90_run_us(mk90 *m, unsigned us) {
    if (!m->speed)
        return;

    m->budget += (uint64_t) us * m->speed;

    while (m->budget >= 1000000) {
        /* Cost is a compatibility instruction budget, not hardware cycles. */

        unsigned cost = m->reset_pending ? 8 : 1;

        if (m->budget < (uint64_t) cost * 1000000)
            break;

        cost = mk90_step(m);
        m->budget -= (uint64_t) cost * 1000000;
        m->time_fraction += (uint64_t) cost * 1000000;

        unsigned elapsed = m->time_fraction / m->speed;

        m->time_fraction %= m->speed;
        mk90_advance_clock(m, elapsed);
    }
}

void mk90_lcd(const mk90 *m, uint8_t pixels[MK90_LCD_W * MK90_LCD_H]) {
    for (unsigned y = 0; y < MK90_LCD_H; y++)
        for (unsigned x = 0; x < MK90_LCD_W; x++) {
            uint16_t a = m->lcd[0] + 2 * ((y % 32) * 15 + x / 8) + y / 32;
            uint8_t  b = a < m->ram_size ? m->ram[a] : 255;

            pixels[y * MK90_LCD_W + x] = (b >> (7 - x % 8)) & 1;
        }
}
