#include "debugger.h"
#include <ctype.h>
#include <string.h>

bool mk90_parse_number(const char *text, unsigned radix, uint32_t maximum, uint32_t *out) {
    if (radix != 8 && radix != 10 && radix != 16)
        return false;

    while (isspace((unsigned char) *text))
        text++;

    if (!*text)
        return false;

    uint32_t n = 0;
    bool     digit = false;

    for (; *text && !isspace((unsigned char) *text); text++) {
        unsigned c = (unsigned char) *text, v;

        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            return false;

        if (v >= radix || v > maximum || n > (maximum - v) / radix)
            return false;

        n = n * radix + v;
        digit = true;
    }

    while (isspace((unsigned char) *text))
        text++;

    if (*text || !digit)
        return false;

    *out = n;
    return true;
}

void mk90_debug_init(mk90_debugger *d) {
    memset(d, 0, sizeof(*d));
}

void mk90_debug_pause(mk90 *m, mk90_debugger *d) {
    d->paused = true;
    d->reason = MK90_DEBUG_PAUSE;
    d->remaining = 0;
    d->temporary = d->skip_once = false;
    m->budget = 0;
}

void mk90_debug_continue(mk90 *m, mk90_debugger *d) {
    d->skip_once = d->paused;
    d->skip_address = m->r[7];
    d->paused = false;
    d->reason = MK90_DEBUG_RUNNING;
    d->remaining = 0;
    d->temporary = false;
    m->budget = 0;
}

void mk90_debug_steps(mk90 *m, mk90_debugger *d, uint32_t count) {
    if (!count)
        return;

    mk90_debug_continue(m, d);
    d->remaining = count;
}

void mk90_debug_until(mk90 *m, mk90_debugger *d, uint16_t address) {
    mk90_debug_continue(m, d);
    d->temporary = true;
    d->temporary_address = address & 0xfffe;
}

int mk90_debug_find(const mk90_debugger *d, uint16_t address) {
    for (unsigned i = 0; i < MK90_BREAKPOINTS; i++)
        if (d->breakpoints[i].used && d->breakpoints[i].address == address)
            return (int) i;
    return -1;
}

int mk90_debug_add(mk90_debugger *d, uint16_t address) {
    if (address & 1)
        return -1;

    int i = mk90_debug_find(d, address);

    if (i >= 0) {
        d->breakpoints[i].enabled = true;
        return i;
    }

    for (i = 0; i < MK90_BREAKPOINTS; i++)
        if (!d->breakpoints[i].used) {
            d->breakpoints[i] = (mk90_breakpoint) { address, true, true };
            return i;
        }
    return -1;
}

static bool stop_before(mk90 *m, mk90_debugger *d) {
    bool skip = d->skip_once && m->r[7] == d->skip_address;

    d->skip_once = false;

    if (skip)
        return false;

    int bp = mk90_debug_find(d, m->r[7]);

    if (d->temporary && m->r[7] == d->temporary_address)
        d->reason = MK90_DEBUG_CURSOR;
    else if (bp >= 0 && d->breakpoints[bp].enabled)
        d->reason = MK90_DEBUG_BREAKPOINT;
    else
        return false;

    d->paused = true;
    d->remaining = 0;
    d->temporary = false;
    m->budget = 0;

    return true;
}

void mk90_debug_run_us(mk90 *m, mk90_debugger *d, unsigned us) {
    if (d->paused || !m->speed)
        return;

    m->budget += (uint64_t) us * m->speed;
    /* Bound each host iteration even for a large requested step count. */
    unsigned limit = 10000;

    while (m->budget >= 1000000 && limit--) {
        unsigned cost = m->reset_pending ? 8 : 1;

        if (m->budget < (uint64_t) cost * 1000000)
            break;

        if (stop_before(m, d))
            break;

        cost = mk90_step(m);
        m->budget -= (uint64_t) cost * 1000000;
        m->time_fraction += (uint64_t) cost * 1000000;

        unsigned elapsed = m->time_fraction / m->speed;

        m->time_fraction %= m->speed;
        mk90_advance_clock(m, elapsed);

        if (d->remaining && --d->remaining == 0) {
            d->paused = true;
            d->reason = MK90_DEBUG_STEPS;
            d->temporary = false;
            m->budget = 0;
            break;
        }
    }
}
