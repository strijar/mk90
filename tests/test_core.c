#define _POSIX_C_SOURCE 200809L
#include "mk90.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static mk90            m;
static const struct tm date = { .tm_year = 126, .tm_mon = 8, .tm_mday = 30, .tm_wday = 3, .tm_hour = 12 };
static void            fresh(void) {
    mk90_destroy(&m);
    mk90_init(&m, 16384, &date);
    m.reset_pending = false;
}
static void word(unsigned a, uint16_t n) {
    m.ram[a] = n;
    m.ram[a + 1] = n >> 8;
}
static uint16_t at(unsigned a) {
    return m.ram[a] | ((uint16_t) m.ram[a + 1] << 8);
}
static void instruction(uint16_t c) {
    word(0x100, c);
    m.r[7] = 0x100;
    mk90_step(&m);
}
static void run_ms(unsigned n) {
    while (n--)
        mk90_run_us(&m, 1000);
}
static void write_io(uint16_t a, uint16_t v) {
    mk90_write(&m, a, v, false);
    mk90_bus_finish(&m);
}

static void cpu_tests(void) {
    fresh();
    m.r[0] = 0x7fff;
    m.r[1] = 1;
    instruction(0060100); /* ADD R1,R0 */
    assert(m.r[0] == 0x8000 && (m.psw & 15) == (MK90_N | MK90_V));
    m.r[0] = 0xffff;
    instruction(0060100);
    assert(m.r[0] == 0 && (m.psw & 15) == (MK90_Z | MK90_C));
    m.r[0] = 0x8000;
    instruction(0160100); /* SUB R1,R0 */
    assert(m.r[0] == 0x7fff && (m.psw & 15) == MK90_V);
    m.r[0] = 1;
    m.r[1] = 2;
    instruction(0020001); /* CMP R0,R1 = R0-R1 */
    assert(m.r[0] == 1 && m.r[1] == 2 && (m.psw & 15) == (MK90_N | MK90_C));
    m.r[1] = 0x80;
    m.r[0] = 0x1234;
    instruction(0110100); /* MOVB R1,R0 */
    assert(m.r[0] == 0xff80);
    instruction(0105000); /* CLRB R0 */
    assert(m.r[0] == 0xff00);

    /* Every byte value for INC/DEC, with carry preserved. */
    for (unsigned x = 0; x < 256; x++)
        for (unsigned carry = 0; carry < 2; carry++) {
            m.r[0] = 0xab00 | x;
            m.psw = carry;
            instruction(0105200);
            unsigned result = (x + 1) & 255;
            unsigned flags = carry | (result == 0 ? MK90_Z : 0) | (result >= 128 ? MK90_N : 0) | (x == 127 ? MK90_V : 0);
            assert(m.r[0] == (0xab00 | result) && (m.psw & 15) == flags);
            m.r[0] = 0xab00 | x;
            m.psw = carry;
            instruction(0105300);
            result = (x - 1) & 255;
            flags = carry | (!result ? MK90_Z : 0) | (result >= 128 ? MK90_N : 0) | (x == 128 ? MK90_V : 0);
            assert(m.r[0] == (0xab00 | result) && (m.psw & 15) == flags);
        }
    fresh();
    m.r[1] = 0x201;
    word(0x200, 0xcafe);
    instruction(0011100);
    assert(m.r[0] == 0xcafe); /* Original emulator masks odd word addresses. */
    m.r[1] = 0x201;
    m.ram[0x201] = 0x88;
    instruction(0112100); /* MOVB (R1)+,R0 */
    assert(m.r[0] == 0xff88 && m.r[1] == 0x202);
    m.r[6] = 0x201;
    instruction(0112600);
    assert(m.r[6] == 0x203);
    word(0x102, 0x1234);
    instruction(0012700);
    assert(m.r[0] == 0x1234 && m.r[7] == 0x104);
    word(0x102, 0x0010);
    word(0x114, 0xbeef);
    instruction(0016700);
    assert(m.r[0] == 0xbeef && m.r[7] == 0x104);
    m.r[1] = 0x202;
    word(0x200, 0x0300);
    word(0x300, 0x4321);
    instruction(0015100);
    assert(m.r[0] == 0x4321 && m.r[1] == 0x200); /* @-(R1) */
    m.psw = 0;
    instruction(0000777);
    assert(m.r[7] == 0x100); /* BR -1 */

    fresh();
    m.r[0] = 0x8000;
    m.r[1] = 0;
    m.r[2] = 0xffff;
    instruction(0071002); /* DIV INT32_MIN/-1 must not invoke C signed overflow. */
    assert(m.psw & MK90_V);
    m.r[0] = 0x8000;
    m.r[1] = 0;
    m.r[2] = 32;
    instruction(0073002);
    assert(m.r[0] == 0xffff && m.r[1] == 0xffff && (m.psw & MK90_C));
    m.r[0] = 0xffff;
    m.r[2] = 16;
    instruction(0072002);
    assert(m.r[0] == 0 && (m.psw & 15) == (MK90_Z | MK90_V | MK90_C));

    fresh();
    m.r[6] = 0x1000;
    word(4, 0x400);
    word(6, 0);
    instruction(0000100); /* JMP R0 is illegal, vector 4. */
    assert(m.r[7] == 0x400 && m.r[6] == 0xffc && at(0xffc) == 0x102);
    instruction(0000002);
    assert(m.r[7] == 0x102 && m.r[6] == 0x1000); /* RTI */
    fresh();
    m.psw = 0;
    m.r[6] = 0x1000;
    word(0xc8, 0x400);
    word(0xca, 0);
    instruction(0000001);
    assert(m.wait);
    mk90_key(&m, 18, false);
    mk90_step(&m);
    assert(!m.wait && m.r[7] == 0x400 && at(0xffc) == 0x102);
    puts("CPU: arithmetic, byte flags, addressing, shifts, traps and interrupts passed");
}

static void device_tests(void) {
    fresh();
    m.rom[0x4000] = 0xaa;
    m.ram[0] = 0x55;
    assert(mk90_read(&m, 0x8000, true) == 255); /* default split reads RAM */
    m.sys1 = 1u << 11;
    assert(mk90_read(&m, 0x8000, true) == 0xaa);
    m.sys2 = 0x2000;
    assert(mk90_read(&m, 0x8000, true) == 255);
    m.fault = false;
    mk90_write(&m, 0xf600, 0, false);
    assert(m.fault);
    mk90_write(&m, 0xe800, 0x200, false);
    assert(m.lcd[0] == 0);
    mk90_bus_finish(&m);
    assert(m.lcd[0] == 0x200);
    uint8_t pixels[120 * 64];
    m.ram[0x200] = 0x80;
    m.ram[0x201] = 1;
    mk90_lcd(&m, pixels);
    assert(pixels[0] == 1 && pixels[1] == 0 && pixels[32 * 120 + 7] == 1 && pixels[120] == 0);
    m.lcd[0] = 0xffff;
    mk90_lcd(&m, pixels);
    assert(pixels[0] == 1); /* address wraps safely */

    fresh();
    mk90_key(&m, 18, false);
    write_io(0xe814, 2);
    write_io(0xe816, 0);
    assert(mk90_read(&m, 0xe810, false) == 0x2b && m.virq[2]);
    mk90_key(&m, 3, true);
    assert(mk90_read(&m, 0xe810, false) == 0x2b);
    assert(mk90_read(&m, 0xe810, false) == 0x23); /* serial shift register */
    mk90_key(&m, 0, true);
    mk90_key(&m, 0, false);
    mk90_read(&m, 0xe810, false);
    assert(mk90_read(&m, 0xe810, false) == 0);

    fresh();
    m.rtc[12] = 0xc0;
    assert(mk90_read(&m, 0xea18, false) == 0x180 && m.rtc[12] == 0xc0);
    mk90_bus_finish(&m);
    assert(m.rtc[12] == 0);
    mk90_write(&m, 0xea14, 6, false);
    assert(m.rtc[10] == 0x7f);
    (void) mk90_read(&m, 0xea14, false);
    assert(m.rtc[10] == 3);
    m.rtc[11] = 0x4f;
    mk90_advance_clock(&m, 122);
    assert(m.evnt && m.virq[0]);
    fresh();
    m.rtc[0] = 59;
    m.rtc[2] = 59;
    m.rtc[4] = 23;
    m.rtc[7] = 31;
    m.rtc[8] = 12;
    m.rtc[9] = 99;
    mk90_rtc_second(&m);
    assert(m.rtc[7] == 1 && m.rtc[8] == 1 && m.rtc[9] == 0);
    m.rtc[11] = 3;
    m.rtc[0] = 0x59;
    m.rtc[2] = 0x59;
    m.rtc[4] = 0x23;
    m.rtc[7] = 0x28;
    m.rtc[8] = 2;
    m.rtc[9] = 0x24;
    mk90_rtc_second(&m);
    assert(m.rtc[7] == 0x29 && m.rtc[8] == 2);
    m.rtc[0] = 0x59;
    m.rtc[2] = 0x59;
    m.rtc[4] = 0x23;
    mk90_rtc_second(&m);
    assert(m.rtc[7] == 1 && m.rtc[8] == 3);
    puts("Devices: memory map, LCD, serial keyboard, deferred RTC and calendar passed");
}

static void boot_and_storage(void) {
    fresh();
    char error[4352];
    assert(mk90_load(&m, MK90_ASSET_DIR, NULL, error, sizeof(error)));
    run_ms(3000);
    assert(m.lcd[0] == 0x200 && m.instructions > 50000);
    uint8_t before[120 * 64], after[120 * 64];
    mk90_lcd(&m, before);
    unsigned black = 0;
    for (unsigned i = 0; i < sizeof(before); i++)
        black += before[i];
    assert(black > 500 && black < 6000);
    /* Select BASIC through the emulated serial keyboard, not a ROM shortcut. */
    mk90_key(&m, 56, false);
    run_ms(150);
    mk90_key(&m, 0, false);
    run_ms(2000);
    mk90_lcd(&m, after);
    assert(memcmp(before, after, sizeof(before)) != 0);
    assert(mk90_write_pbm(&m, "basic.pbm"));

    /* Serial protocol: select card, write address, write byte. */
    write_io(0xe814, 8);
    write_io(0xe816, 0xa0);
    write_io(0xe810, 0);
    write_io(0xe810, 7);
    write_io(0xe816, 0xc0);
    write_io(0xe810, 0xa5);
    assert(m.smp[0].data[7] == 0xa5 && m.smp[0].position == 8 && m.smp[0].dirty);
    char temp[] = "/tmp/mk90-test-XXXXXX";
    assert(mkdtemp(temp));
    assert(mk90_save(&m, temp, error, sizeof(error)) && !m.smp[0].dirty);
    fresh();
    assert(mk90_load(&m, MK90_ASSET_DIR, temp, error, sizeof(error)));
    assert(m.smp[0].data[7] == 0xa5);
    char path[128];
    snprintf(path, sizeof(path), "%s/smp0.bin", temp);
    assert(unlink(path) == 0 && rmdir(temp) == 0);
    mk90_destroy(&m);
    puts("Integration: ROM boots, BASIC selection changes LCD, card writes survive reload");
}

int main(void) {
    cpu_tests();
    device_tests();
    boot_and_storage();
    return 0;
}
