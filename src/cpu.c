/* C port of emul_src/{cpu,decoder,exec,srcdst}.pas.
 * Original instruction/addressing code credits Ovsienko V. A.
 * Explicit unsigned arithmetic replaces Pascal's wrapping words and pointers.
 */

#include "mk90.h"

typedef struct {
    uint16_t address;
    unsigned reg;
    bool     memory;
} operand;

static int32_t signed16(uint16_t n) {
    return n < 0x8000 ? n : (int32_t) n - 65536;
}

static uint16_t extend8(uint16_t n) {
    return n & 0x80 ? n | 0xff00 : n & 255;
}

static operand resolve(mk90 *m, unsigned spec, bool byte) {
    operand  o = { 0, spec & 7, (spec & 070) != 0 };
    unsigned mode = (spec >> 3) & 7, reg = o.reg;
    unsigned increment = byte && reg < 6 ? 1 : 2;
    uint16_t a = m->r[reg], displacement;

    switch (mode) {
        case 0:
            return o;

        case 1:
            break;

        case 2:
            m->r[reg] += increment;
            break;

        case 3:
            m->r[reg] += 2;
            a = mk90_read(m, a & 0xfffe, false);
            break;

        case 4:
            m->r[reg] -= increment;
            a = m->r[reg];
            break;

        case 5:
            m->r[reg] -= 2;
            a = mk90_read(m, m->r[reg] & 0xfffe, false);
            break;

        case 6:
        case 7:
            a = m->r[7];
            m->r[7] += 2;
            displacement = mk90_read(m, a, false);
            a = m->r[reg] + displacement;
            if (mode == 7)
                a = mk90_read(m, a & 0xfffe, false);
            break;
    }

    o.address = byte ? a : a & 0xfffe;
    return o;
}

static uint16_t read_op(mk90 *m, operand o, bool byte) {
    uint16_t n = o.memory ? mk90_read(m, o.address, byte) : m->r[o.reg];

    return byte ? n & 255 : n;
}

static void write_op(mk90 *m, operand o, uint16_t n, bool byte) {
    if (o.memory)
        mk90_write(m, o.address, n, byte);
    else
        m->r[o.reg] = byte ? (m->r[o.reg] & 0xff00) | (n & 255) : n;
}

static uint16_t source(mk90 *m, unsigned spec, bool byte) {
    return read_op(m, resolve(m, spec, byte), byte);
}

static void nz(mk90 *m, uint16_t n, bool byte) {
    n &= byte ? 255 : 65535;
    if (!n)
        m->psw |= MK90_Z;
    if (n & (byte ? 0x80 : 0x8000))
        m->psw |= MK90_N;
}

static void push(mk90 *m, uint16_t n) {
    m->r[6] -= 2;
    mk90_write(m, m->r[6] & 0xfffe, n, false);
}

static uint16_t pop(mk90 *m) {
    uint16_t a = m->r[6];

    m->r[6] += 2;

    return mk90_read(m, a & 0xfffe, false);
}

static void trap(mk90 *m, uint16_t vector) {
    push(m, m->psw);
    push(m, m->r[7]);
    m->r[7] = mk90_read(m, vector, false) & 0xfffe;
    m->psw = mk90_read(m, vector + 2, false);
}

static void arithmetic(mk90 *m, uint16_t a, uint16_t b, uint16_t r, bool subtract, bool byte) {
    unsigned sign = byte ? 0x80 : 0x8000;
    unsigned mask = byte ? 255 : 65535;

    m->psw &= ~15;
    nz(m, r, byte);

    if (subtract) {
        if (a < b)
            m->psw |= MK90_C;
        if ((a ^ b) & (a ^ r) & sign)
            m->psw |= MK90_V;
    } else {
        if ((unsigned) a + b > mask)
            m->psw |= MK90_C;
        if (~(a ^ b) & (a ^ r) & sign)
            m->psw |= MK90_V;
    }
}

static uint16_t execute(mk90 *m, bool *rtt) {
    uint16_t c = m->opcode, s, d, x;
    unsigned top = c >> 12, reg = (c >> 6) & 7;
    bool     byte = top >= 9 && top <= 13;
    operand  o;

    /* Two-operand instructions. Source must be fetched before resolving dest. */

    if ((top >= 1 && top <= 6) || (top >= 9 && top <= 14)) {
        unsigned op = top & 7;

        s = source(m, (c >> 6) & 63, byte);
        o = resolve(m, c & 63, byte);

        if (op == 1) {
            write_op(m, o, byte && !o.memory ? extend8(s) : s, byte && o.memory);
            m->psw &= ~14;
            nz(m, s, byte);
            return 0;
        }

        d = read_op(m, o, byte);

        switch (op) {
            case 2:
                x = s - d;
                arithmetic(m, s, d, x, true, byte);
                return 0;

            case 3:
                x = s & d;
                break;

            case 4:
                x = d & ~s;
                break;

            case 5:
                x = d | s;
                break;

            case 6:
                x = top == 14 ? d - s : d + s;
                arithmetic(m, d, s, x, top == 14, false);
                write_op(m, o, x, false);
                return 0;

            default:
                return 8;
        }

        if (op != 3)
            write_op(m, o, x, byte);
        m->psw &= ~14;
        nz(m, x, byte);

        return 0;
    }

    /* Conditional branches. */
    unsigned branch = c >> 8;

    bool     n = (m->psw & MK90_N) != 0, z = (m->psw & MK90_Z) != 0;
    bool     v = (m->psw & MK90_V) != 0, carry = (m->psw & MK90_C) != 0;
    bool     take;

    switch (branch) {
        case 1:
            take = true;
            break;

        case 2:
            take = !z;
            break;

        case 3:
            take = z;
            break;

        case 4:
            take = n == v;
            break;

        case 5:
            take = n != v;
            break;

        case 6:
            take = !z && n == v;
            break;

        case 7:
            take = z || n != v;
            break;

        case 0x80:
            take = !n;
            break;

        case 0x81:
            take = n;
            break;

        case 0x82:
            take = !carry && !z;
            break;

        case 0x83:
            take = carry || z;
            break;

        case 0x84:
            take = !v;
            break;

        case 0x85:
            take = v;
            break;

        case 0x86:
            take = !carry;
            break;

        case 0x87:
            take = carry;
            break;

        default:
            take = false;
            branch = 0;
            break;
    }

    if (branch) {
        if (take)
            m->r[7] += 2 * ((c & 0x80) ? (int) (c & 255) - 256 : (int) (c & 255));
        return 0;
    }

    if (c <= 6) {
        switch (c) {
            case 0:
                trap(m, 4);
                m->psw |= MK90_H;
                return 0;

            case 1:
                m->wait = true;
                return 0;

            case 6:
                *rtt = true; /* fall through */
            case 2:
                m->r[7] = pop(m) & 0xfffe;
                m->psw = pop(m);
                return 0;

            case 3:
                return 12;

            case 4:
                return 16;

            case 5:
                m->reset_pending = true;
                return 0;
        }
    }

    if ((c & 0177700) == 0000100) { /* JMP */
        if (!(c & 070))
            return 4;
        m->r[7] = resolve(m, c & 63, false).address;

        return 0;
    }

    if ((c & 0177770) == 0000200) { /* RTS */
        m->r[7] = m->r[c & 7] & 0xfffe;
        m->r[c & 7] = pop(m);
        return 0;
    }

    if ((c & 0177740) == 0000240) {
        if (c & 020)
            m->psw |= c & 15;
        else
            m->psw &= ~(c & 15);
        return 0;
    }

    if ((c & 0177000) == 0004000) { /* JSR */
        if (!(c & 070))
            return 4;

        o = resolve(m, c & 63, false);
        push(m, m->r[reg]);
        m->r[reg] = m->r[7];
        m->r[7] = o.address;
        return 0;
    }

    if ((c & 0177000) == 0077000) { /* SOB */
        if (--m->r[reg])
            m->r[7] -= 2 * (c & 63);
        return 0;
    }

    if ((c & 0177400) == 0104000)
        return 24;

    if ((c & 0177400) == 0104400)
        return 28;

    if ((c & 0170000) == 0070000) {
        unsigned op = (c >> 9) & 7;

        if (op == 5) {
            if (!(c & 0xe0))
                m->psw |= MK90_H;
            return 8;
        }

        if (op == 6)
            return 8;

        if (op == 4) { /* XOR */
            o = resolve(m, c & 63, false);
            x = read_op(m, o, false) ^ m->r[reg];
            write_op(m, o, x, false);
            m->psw &= ~14;
            nz(m, x, false);
            return 0;
        }

        /* ASH/ASHC sample register before resolving the count, as in Pascal. */

        uint32_t bits = op == 3 ? ((uint32_t) m->r[reg] << 16) | m->r[reg | 1] : m->r[reg];
        s = source(m, c & 63, false);
        m->psw &= ~15;

        if (op == 0) {
            int32_t product = signed16(m->r[reg]) * signed16(s);

            m->r[reg] = (uint32_t) product >> 16;
            m->r[reg | 1] = (uint16_t) product;

            if (product < 0)
                m->psw |= MK90_N;

            if (!product)
                m->psw |= MK90_Z;

            if (product < -32768 || product > 32767)
                m->psw |= MK90_C;
        } else if (op == 1) {
            int32_t divisor = signed16(s);

            if (!divisor)
                m->psw |= MK90_V | MK90_C;
            else {
                int64_t dividend = (int64_t) signed16(m->r[reg]) * 65536 + m->r[reg | 1];
                int64_t quotient = dividend / divisor;

                m->r[reg | 1] = (uint16_t) (dividend % divisor);
                m->r[reg] = (uint16_t) quotient;

                if (quotient < 0)
                    m->psw |= MK90_N;

                if (!quotient)
                    m->psw |= MK90_Z;

                if (quotient < -32768 || quotient > 32767)
                    m->psw |= MK90_V;
            }
        } else {
            unsigned count = s & 63;
            bool     right = count >= 32;

            if (right)
                count = 64 - count;

            uint32_t sign = op == 3 ? UINT32_C(0x80000000) : 0x8000;
            uint32_t mask = op == 3 ? UINT32_MAX : 0xffff;
            bool     overflow = false, out = false;

            while (count--) {
                if (right) {
                    out = bits & 1;
                    bits = (bits >> 1) | (bits & sign);
                } else {
                    out = (bits & sign) != 0;
                    bits = (bits << 1) & mask;
                    overflow |= out != ((bits & sign) != 0);
                }
            }

            if (op == 3) {
                m->r[reg] = bits >> 16;
                m->r[reg | 1] = (uint16_t) bits;
            } else
                m->r[reg] = (uint16_t) bits;

            if (bits & sign)
                m->psw |= MK90_N;

            if (!bits)
                m->psw |= MK90_Z;

            if (out)
                m->psw |= MK90_C;

            if (overflow)
                m->psw |= MK90_V;
        }
        return 0;
    }

    unsigned op = c & 0177700;

    if (op == 0006400) { /* MARK */
        m->r[6] = m->r[7] + 2 * (c & 63);
        m->r[7] = m->r[5] & 0xfffe;
        m->r[5] = pop(m);
        return 0;
    }

    if (op == 0106400) { /* MTPS */
        x = source(m, c & 63, true);
        m->psw = (m->psw & 0xff10) | (x & 0xef);
        return 0;
    }

    if (op == 0106700) { /* MFPS */
        x = m->psw & 255;
        o = resolve(m, c & 63, true);
        write_op(m, o, o.memory ? x : extend8(x), o.memory);
        m->psw &= ~14;
        nz(m, x, true);
        return 0;
    }

    if (op == 0006700) { /* SXT */
        o = resolve(m, c & 63, false);
        x = n ? 65535 : 0;
        write_op(m, o, x, false);
        m->psw &= ~(MK90_Z | MK90_V);
        if (!n)
            m->psw |= MK90_Z;
        return 0;
    }

    if (op == 0000300) { /* SWAB */
        o = resolve(m, c & 63, false);
        d = read_op(m, o, false);
        x = (d << 8) | (d >> 8);
        write_op(m, o, x, false);
        m->psw &= ~15;
        nz(m, x, true);
        return 0;
    }

    byte = (c & 0100000) != 0;
    op &= 077700;

    if (op < 0005000 || op > 0006300)
        return 8;

    unsigned which = (op - 0005000) >> 6;
    unsigned sign = byte ? 0x80 : 0x8000, mask = byte ? 255 : 65535;

    o = resolve(m, c & 63, byte);
    d = which == 0 ? 0 : read_op(m, o, byte);
    x = d;
    m->psw &= ~14;

    switch (which) {
        case 0:
            x = 0;
            m->psw &= ~MK90_C;
            break;

        case 1:
            x = ~d;
            m->psw |= MK90_C;
            break;

        case 2:
            x = (d + 1) & mask;
            if (x == sign)
                m->psw |= MK90_V;
            break;

        case 3:
            x = (d - 1) & mask;
            if (x == sign - 1)
                m->psw |= MK90_V;
            break;

        case 4:
            x = (0u - d) & mask;
            m->psw &= ~MK90_C;

            if (x)
                m->psw |= MK90_C;
            if (x == sign)
                m->psw |= MK90_V;
            break;

        case 5:
            x = (d + carry) & mask;
            m->psw &= ~MK90_C;

            if (carry && d == mask)
                m->psw |= MK90_C;

            if (carry && d == sign - 1)
                m->psw |= MK90_V;
            break;

        case 6:
            x = (d - carry) & mask;
            m->psw &= ~MK90_C;

            if (carry && !d)
                m->psw |= MK90_C;
            if (carry && d == sign)
                m->psw |= MK90_V;
            break;

        case 7:
            m->psw &= ~MK90_C;
            break;

        case 8:
        case 9:
        case 10:
        case 11: {
            bool out;

            if (which == 8 || which == 10) {
                out = (d & 1) != 0;
                x = (d >> 1) | (which == 8 ? (carry ? sign : 0) : d & sign);
            } else {
                out = (d & sign) != 0;
                x = ((d << 1) | (which == 9 && carry)) & mask;
            }

            m->psw &= ~MK90_C;

            if (out)
                m->psw |= MK90_C;

            if (((x & sign) != 0) != out)
                m->psw |= MK90_V;
            break;
        }
    }

    if (which != 7)
        write_op(m, o, x, byte);

    nz(m, x, byte);
    return 0;
}

unsigned mk90_step(mk90 *m) {
    unsigned cost = m->reset_pending ? 8 : 1;

    m->reset_pending = false;

    if (m->fault || m->halt_irq || m->evnt || m->virq[0] || m->virq[1] || m->virq[2])
        m->wait = false;

    if (m->psw & MK90_H)
        m->fault = m->halt_irq = false;

    if (m->wait)
        return cost;

    uint16_t vector = 0;

    if (m->fault) {
        vector = 4;
        m->fault = false;
    } else if (m->halt_irq) {
        vector = 0xe002;
        m->halt_irq = false;
    } else if (!(m->psw & MK90_I)) {
        if (m->evnt) {
            vector = 0x40;
            m->evnt = false;
        } else
            for (unsigned i = 0; i < 3; i++)
                if (m->virq[i]) {
                    vector = 0xc0 + 4 * i;
                    m->virq[i] = false;
                    break;
                }
    }

    if (!vector) {
        m->opcode = source(m, 027, false);

        bool rtt = false;

        vector = execute(m, &rtt);
        m->r[7] &= 0xfffe;
        mk90_bus_finish(m);

        if ((m->psw & MK90_T) && !vector && !rtt)
            vector = 12;
        m->instructions++;
    }

    if (vector)
        trap(m, vector);
    return cost;
}
