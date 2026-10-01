#define _POSIX_C_SOURCE 200809L
#include "debugger.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static mk90 m;
static void fresh(void) {
    mk90_destroy(&m);
    const struct tm date = { .tm_year = 126, .tm_mon = 9, .tm_mday = 1 };
    mk90_init(&m, 16384, &date);
    m.reset_pending = false;
    m.r[7] = 0x100;
}
static void word(uint16_t a, uint16_t v) {
    m.ram[a] = v;
    m.ram[a + 1] = v >> 8;
}

static void inspection(void) {
    fresh();
    m.rtc[12] = 0xf0;
    m.rtc_index = 10;
    m.rtc_word = 6;
    m.shift = 0x1234;
    m.selected = true;
    m.requests = 0xabcd;
    m.smp[0].position = 123;
    m.smp[0].command = 0xd0;
    mk90     before = m;
    uint16_t value;
    assert(mk90_peek(&m, 0xea18, false, MK90_VIEW_CPU, &value) && value == 0x1e0);
    assert(mk90_peek(&m, 0xea14, false, MK90_VIEW_CPU, &value) && value == 6);
    assert(mk90_peek(&m, 0xe810, false, MK90_VIEW_CPU, &value) && value == 0x1234);
    assert(mk90_peek(&m, 0xe816, false, MK90_VIEW_CPU, &value) && value == 0x1234);
    for (unsigned space = 0; space < 3; space++)
        for (unsigned a = 0; a < 65536; a++) {
            mk90_peek(&m, a, true, (mk90_memory_view) space, &value);
            mk90_peek(&m, a, false, (mk90_memory_view) space, &value);
        }
    assert(!memcmp(&before, &m, sizeof(m)));
    m.rom[0x4000] = 0xab;
    assert(mk90_peek(&m, 0x8000, true, MK90_VIEW_ROM, &value) && value == 0xab);
    assert(!mk90_peek(&m, 0x8000, true, MK90_VIEW_CPU, &value));
    m.sys1 = 1 << 11;
    assert(mk90_peek(&m, 0x8000, true, MK90_VIEW_CPU, &value) && value == 0xab);
    assert(!mk90_peek(&m, 0xffff, false, MK90_VIEW_ROM, &value));
    puts("Inspection: all addresses, CPU/RAM/ROM views, no I/O side effects");
}

static void disassembler(void) {
    fresh();
    mk90_disassembly d;
    word(0x100, 0012767);
    word(0x102, 0x1234);
    word(0x104, 8);
    mk90_disassemble(&m, 0x100, 16, &d);
    assert(d.count == 3 && d.next == 0x106 && !strcmp(d.text, "MOV #1234,010E"));
    word(0x100, 0000777);
    mk90_disassemble(&m, 0x100, 8, &d);
    assert(!strcmp(d.text, "BR 000400") && d.next == 0x102);
    word(0x100, 0077201);
    mk90_disassemble(&m, 0x100, 16, &d);
    assert(!strcmp(d.text, "SOB R2,0100"));
    word(0x100, 0112700);
    word(0x102, 0x80);
    mk90_disassemble(&m, 0x100, 16, &d);
    assert(!strcmp(d.text, "MOVB #0080,R0"));
    word(0x100, 0xffff);
    mk90_disassemble(&m, 0x100, 16, &d);
    assert(!strcmp(d.text, ".WORD FFFF"));
    mk90_disassemble(&m, 0xfffe, 16, &d);
    assert(d.next == 0 && !d.valid[0]);
    /* Exercise every opcode, including all address modes and extension lengths. */
    for (unsigned opcode = 0; opcode < 65536; opcode++) {
        word(0x100, opcode);
        mk90 before = m;
        mk90_disassemble(&m, 0x100, 8, &d);
        assert(d.count >= 1 && d.count <= 3 && d.next == 0x100 + 2 * d.count && d.text[0]);
        assert(!memcmp(&before, &m, sizeof(m)));
    }
    uint32_t number;
    assert(mk90_parse_number("177777", 8, 65535, &number) && number == 65535);
    assert(mk90_parse_number(" fFfF ", 16, 65535, &number) && number == 65535);
    assert(!mk90_parse_number("200000", 8, 65535, &number));
    assert(!mk90_parse_number("8", 8, 65535, &number));
    assert(!mk90_parse_number("-1", 16, 65535, &number));
    assert(!mk90_parse_number("", 16, 65535, &number));
    assert(!mk90_parse_number("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", 16, 65535, &number));
    puts("Disassembler: all 65536 opcodes, extension words, PC-relative operands, numeric parsing");
}

static void execution(void) {
    fresh();
    mk90_debugger d;
    mk90_debug_init(&d);
    word(0x100, 0005200);
    word(0x102, 0005200);
    word(0x104, 0005200);
    word(0x106, 0000777);
    int index = mk90_debug_add(&d, 0x102);
    assert(index >= 0);
    mk90_debug_run_us(&m, &d, 1000);
    assert(d.paused && d.reason == MK90_DEBUG_BREAKPOINT && m.r[7] == 0x102 && m.r[0] == 1);
    mk90 before = m;
    mk90_debug_run_us(&m, &d, 1000000);
    assert(!memcmp(&before, &m, sizeof(m)));
    mk90_debug_steps(&m, &d, 1);
    mk90_debug_run_us(&m, &d, 1000);
    assert(d.paused && d.reason == MK90_DEBUG_STEPS && m.r[7] == 0x104 && m.r[0] == 2);
    mk90_debug_until(&m, &d, 0x106);
    mk90_debug_run_us(&m, &d, 1000);
    assert(d.paused && d.reason == MK90_DEBUG_CURSOR && m.r[0] == 3 && !d.temporary);
    int loop = mk90_debug_add(&d, 0x106);
    assert(loop >= 0);
    uint64_t count = m.instructions;
    mk90_debug_continue(&m, &d);
    mk90_debug_run_us(&m, &d, 1000);
    assert(d.paused && m.instructions == count + 1 && d.reason == MK90_DEBUG_BREAKPOINT);
    d.breakpoints[loop].enabled = false;
    mk90_debug_steps(&m, &d, 123);
    mk90_debug_run_us(&m, &d, 10000);
    assert(d.paused && m.instructions == count + 124);
    assert(mk90_debug_add(&d, 0x101) < 0);
    for (unsigned i = 0; i < MK90_BREAKPOINTS - 2; i++)
        assert(mk90_debug_add(&d, 0x200 + i * 2) >= 0);
    assert(mk90_debug_add(&d, 0x800) < 0);
    fresh();
    mk90_debug_init(&d);
    word(0x100, 0000777);
    mk90 normal = m;
    for (unsigned i = 0; i < 1000; i++) {
        mk90_run_us(&normal, 1000);
        mk90_debug_run_us(&m, &d, 1000);
    }
    assert(!memcmp(&normal, &m, sizeof(m)));
    /* WAIT and interrupt entry are CPU steps, not invented extra instructions. */
    fresh();
    mk90_debug_init(&d);
    mk90_debug_pause(&m, &d);
    word(0x100, 1);
    mk90_debug_steps(&m, &d, 1);
    mk90_debug_run_us(&m, &d, 1000);
    assert(m.wait && d.paused);
    count = m.instructions;
    mk90_debug_steps(&m, &d, 1);
    mk90_debug_run_us(&m, &d, 1000);
    assert(d.paused && m.wait && m.instructions == count);
    puts("Execution: breakpoints, one-time resume skip, N steps, run-to-cursor, WAIT and clock parity");
}

static void cards(void) {
    fresh();
    char dir[] = "/tmp/mk90-debug-test-XXXXXX";
    assert(mkdtemp(dir));
    char input[128], output[128], bad[128], error[4352];
    snprintf(input, sizeof(input), "%s/input.bin", dir);
    snprintf(output, sizeof(output), "%s/output.bin", dir);
    snprintf(bad, sizeof(bad), "%s/bad.bin", dir);
    uint8_t data[257];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t) (i * 37);
    FILE *f = fopen(input, "wb");
    assert(f && fwrite(data, 1, sizeof(data), f) == sizeof(data) && !fclose(f));
    assert(mk90_smp_import(&m, 0, input, error, sizeof(error)));
    assert(m.smp[0].size == sizeof(data) && !memcmp(m.smp[0].data, data, sizeof(data)) && m.smp[0].dirty);
    assert(m.smp[0].position == 0 && m.smp[0].command == 0 && m.smp[0].mask == 0xffff);
    mk90 before = m;
    assert(!mk90_smp_import(&m, 0, bad, error, sizeof(error)) && !memcmp(&before, &m, sizeof(m)));
    f = fopen(bad, "wb");
    assert(f && !fclose(f));
    assert(!mk90_smp_import(&m, 0, bad, error, sizeof(error)) && !memcmp(&before, &m, sizeof(m)));
    assert(mk90_smp_export(&m, 0, output, false, error, sizeof(error)) && m.smp[0].dirty);
    assert(!mk90_smp_export(&m, 0, output, false, error, sizeof(error)));
    assert(mk90_smp_import(&m, 1, output, error, sizeof(error)));
    assert(m.smp[1].size == sizeof(data) && !memcmp(m.smp[1].data, data, sizeof(data)));
    m.smp[1].data[0] ^= 255;
    assert(mk90_smp_export(&m, 1, output, true, error, sizeof(error)));
    f = fopen(input, "rb");
    assert(f && fgetc(f) == data[0] && !fclose(f)); /* input untouched */
    f = fopen(bad, "wb");
    assert(f && !ftruncate(fileno(f), 65536) && !fclose(f));
    assert(mk90_smp_import(&m, 1, bad, error, sizeof(error)) && m.smp[1].mask == 0xffffff && m.smp[1].size == 65536);
    f = fopen(bad, "wb");
    assert(f && !ftruncate(fileno(f), 0x1000001) && !fclose(f));
    before = m;
    assert(!mk90_smp_import(&m, 1, bad, error, sizeof(error)) && !memcmp(&before, &m, sizeof(m)));
    assert(!mk90_smp_import(&m, 2, input, error, sizeof(error)));
    assert(!mk90_smp_export(&m, 2, output, false, error, sizeof(error)));
    assert(!unlink(input) && !unlink(output) && !unlink(bad) && !rmdir(dir));
    mk90_destroy(&m);
    puts("SMP: transactional import, exact roundtrip, overwrite protection, ROM cards and invalid images");
}
int main(void) {
    inspection();
    disassembler();
    execution();
    cards();
    return 0;
}
