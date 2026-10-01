/* Disassembler adapted from emul_src/pdp11dis.pas, whose disassembler
 * credits Martin Minow's pinst.c. All fetches use the side-effect-free view.
 */

#include "debugger.h"
#include <stdio.h>
#include <string.h>

typedef enum { ILLOP,
               NONE,
               RTS,
               DOUBLE,
               ADD,
               SWBYTE,
               SINGLE,
               JSR,
               MUL,
               BR,
               SOB,
               SPL,
               MARK,
               TRAP,
               CODE,
               DATA
} kind;

static const struct {
    uint16_t    mask, op;
    const char *name;
    kind        type;
} table[] = {
    { 0xFFFF, 0x0000, "HALT",  NONE   },
    { 0xFFFF, 0x0001, "WAIT",  NONE   },
    { 0xFFFF, 0x0002, "RTI",   NONE   },
    { 0xFFFF, 0x0003, "BPT",   NONE   },
    { 0xFFFF, 0x0004, "IOT",   NONE   },
    { 0xFFFF, 0x0005, "RESET", NONE   },
    { 0xFFFF, 0x0006, "RTT",   NONE   },
    { 0xFFE0, 0x0000, "ILLOP", ILLOP  },
    { 0xFFC0, 0x0040, "JMP",   SINGLE },
    { 0xFFF8, 0x0080, "RTS",   RTS    },
    { 0xFFF0, 0x0088, "ILLOP", ILLOP  },
    { 0xFFF8, 0x0098, "SPL",   SPL    },
    { 0xFFFF, 0x00A0, "NOP",   NONE   },
    { 0xFFFF, 0x00A1, "CLC",   NONE   },
    { 0xFFF0, 0x00A0, "CLEAR", CODE   },
    { 0xFFFF, 0x00B1, "SEC",   NONE   },
    { 0xFFF0, 0x00B0, "SET",   CODE   },
    { 0xFFC0, 0x00C0, "SWAB",  SINGLE },
    { 0xFF00, 0x0100, "BR",    BR     },
    { 0xFF00, 0x0200, "BNE",   BR     },
    { 0xFF00, 0x0300, "BEQ",   BR     },
    { 0xFF00, 0x0400, "BGE",   BR     },
    { 0xFF00, 0x0500, "BLT",   BR     },
    { 0xFF00, 0x0600, "BGT",   BR     },
    { 0xFF00, 0x0700, "BLE",   BR     },
    { 0xFE00, 0x0800, "JSR",   JSR    },
    { 0x7FC0, 0x0A00, "CLR",   SWBYTE },
    { 0x7FC0, 0x0A40, "COM",   SWBYTE },
    { 0x7FC0, 0x0A80, "INC",   SWBYTE },
    { 0x7FC0, 0x0AC0, "DEC",   SWBYTE },
    { 0x7FC0, 0x0B00, "NEG",   SWBYTE },
    { 0x7FC0, 0x0B40, "ADC",   SWBYTE },
    { 0x7FC0, 0x0B80, "SBC",   SWBYTE },
    { 0x7FC0, 0x0BC0, "TST",   SWBYTE },
    { 0x7FC0, 0x0C00, "ROR",   SWBYTE },
    { 0x7FC0, 0x0C40, "ROL",   SWBYTE },
    { 0x7FC0, 0x0C80, "ASR",   SWBYTE },
    { 0x7FC0, 0x0CC0, "ASL",   SWBYTE },
    { 0xFFC0, 0x0D00, "MARK",  MARK   },
    { 0xFFC0, 0x0D40, "MFPI",  SINGLE },
    { 0xFFC0, 0x0D80, "MTPI",  SINGLE },
    { 0xFFC0, 0x0DC0, "SXT",   SINGLE },
    { 0xFE00, 0x0E00, "ILLOP", ILLOP  },
    { 0x7000, 0x1000, "MOV",   DOUBLE },
    { 0x7000, 0x2000, "CMP",   DOUBLE },
    { 0x7000, 0x3000, "BIT",   DOUBLE },
    { 0x7000, 0x4000, "BIC",   DOUBLE },
    { 0x7000, 0x5000, "BIS",   DOUBLE },
    { 0xF000, 0x6000, "ADD",   ADD    },
    { 0xF000, 0xE000, "SUB",   ADD    },
    { 0xFE00, 0x7000, "MUL",   MUL    },
    { 0xFE00, 0x7200, "DIV",   MUL    },
    { 0xFE00, 0x7600, "ASHC",  MUL    },
    { 0xFE00, 0x7400, "ASH",   MUL    },
    { 0xFE00, 0x7800, "XOR",   JSR    },
    { 0xFFF8, 0x7A00, "FADD",  RTS    },
    { 0xFFF8, 0x7A08, "FSUB",  RTS    },
    { 0xFFF8, 0x7A10, "FMUL",  RTS    },
    { 0xFFF8, 0x7A18, "FDIV",  RTS    },
    { 0xFE00, 0x7E00, "SOB",   SOB    },
    { 0xF800, 0x7800, "ILLOP", ILLOP  },
    { 0xFF00, 0x8000, "BPL",   BR     },
    { 0xFF00, 0x8100, "BMI",   BR     },
    { 0xFF00, 0x8600, "BCC",   BR     },
    { 0xFF00, 0x8600, "BHIS",  BR     },
    { 0xFF00, 0x8200, "BHI",   BR     },
    { 0xFF00, 0x8300, "BLOS",  BR     },
    { 0xFF00, 0x8400, "BVC",   BR     },
    { 0xFF00, 0x8500, "BVS",   BR     },
    { 0xFF00, 0x8700, "BCS",   BR     },
    { 0xFF00, 0x8700, "BLO",   BR     },
    { 0xFF00, 0x8800, "EMT",   TRAP   },
    { 0xFF00, 0x8900, "TRAP",  TRAP   },
    { 0xFFC0, 0x8D00, "MTPS",  SINGLE },
    { 0xFFC0, 0x8D40, "MFPD",  SINGLE },
    { 0xFFC0, 0x8D80, "MTPD",  SINGLE },
    { 0xFFC0, 0x8DC0, "MFPS",  SINGLE },
    { 0x0000, 0x0000, "ILLOP", ILLOP  },
    { 0x0000, 0xFFFF, ".WORD", DATA   },
};

static const char *register_name(unsigned r) {
    static const char *names[] = { "R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC" };
    return names[r & 7];
}

static uint16_t fetch(const mk90 *m, mk90_disassembly *d) {
    uint16_t v;
    bool     valid = mk90_peek(m, d->next, false, MK90_VIEW_CPU, &v);

    if (d->count < 3) {
        d->words[d->count] = v;
        d->valid[d->count] = valid;
        d->count++;
    }

    d->next += 2;
    return v;
}

static void number(char *s, size_t size, unsigned radix, uint16_t v) {
    snprintf(s, size, radix == 16 ? "%04X" : "%06o", v);
}

static void operand(const mk90 *m, mk90_disassembly *d, unsigned spec, unsigned radix, char *out, size_t size) {
    unsigned    r = spec & 7, mode = (spec >> 3) & 7;
    const char *name = register_name(r), *at = (mode & 1) ? "@" : "";
    char        value[16];

    switch (mode) {
        case 0:
            snprintf(out, size, "%s", name);
            break;

        case 1:
            snprintf(out, size, "(%s)", name);
            break;

        case 2:
        case 3:
            if (r == 7) {
                number(value, sizeof(value), radix, fetch(m, d));
                snprintf(out, size, "%s#%s", at, value);
            } else
                snprintf(out, size, "%s(%s)+", at, name);
            break;

        case 4:
        case 5:
            snprintf(out, size, "%s-(%s)", at, name);
            break;

        case 6:
        case 7: {
            uint16_t offset = fetch(m, d);
            number(value, sizeof(value), radix, r == 7 ? (uint16_t) (d->next + offset) : offset);

            if (r == 7)
                snprintf(out, size, "%s%s", at, value);
            else
                snprintf(out, size, "%s%s(%s)", at, value, name);
            break;
        }
    }
}

void mk90_disassemble(const mk90 *m, uint16_t address, unsigned radix, mk90_disassembly *d) {
    memset(d, 0, sizeof(*d));
    d->address = d->next = address;
    uint16_t code = fetch(m, d);

    if (!d->valid[0]) {
        snprintf(d->text, sizeof(d->text), "<unmapped>");
        return;
    }

    unsigned i = 0;

    while (i + 1 < sizeof(table) / sizeof(table[0]) && (code & table[i].mask) != table[i].op)
        i++;

    kind k = table[i].type;
    char a[48] = "", b[48] = "", args[100] = "", name[16];

    snprintf(name, sizeof(name), "%s%s", table[i].name, (k == DOUBLE || k == SWBYTE) && (code & 0x8000) ? "B" : "");

    switch (k) {
        case RTS:
            snprintf(args, sizeof(args), "%s", register_name(code));
            break;

        case SINGLE:
        case SWBYTE:
            operand(m, d, code, radix, args, sizeof(args));
            break;

        case DOUBLE:
        case ADD:
            operand(m, d, code >> 6, radix, a, sizeof(a));
            operand(m, d, code, radix, b, sizeof(b));
            snprintf(args, sizeof(args), "%s,%s", a, b);
            break;

        case JSR:
            operand(m, d, code, radix, a, sizeof(a));
            snprintf(args, sizeof(args), "%s,%s", register_name(code >> 6), a);
            break;

        case MUL:
            operand(m, d, code, radix, a, sizeof(a));
            snprintf(args, sizeof(args), "%s,%s", a, register_name(code >> 6));
            break;

        case BR:
            number(args, sizeof(args), radix, (uint16_t) (d->next + 2 * ((code & 128) ? (int) (code & 255) - 256 : (int) (code & 255))));
            break;

        case SOB:
            number(a, sizeof(a), radix, (uint16_t) (d->next - 2 * (code & 63)));
            snprintf(args, sizeof(args), "%s,%s", register_name(code >> 6), a);
            break;

        case SPL:
            number(args, sizeof(args), radix, code & 7);
            break;

        case MARK:
            number(args, sizeof(args), radix, code & 63);
            break;

        case TRAP:
            number(args, sizeof(args), radix, code & 255);
            break;

        case CODE: {
            unsigned n = 0;

            for (unsigned bit = 0; bit < 4; bit++)
                if (code & (1u << bit))
                    args[n++] = "CVZN"[bit];
            break;
        }

        case ILLOP:
        case DATA:
            strcpy(name, ".WORD");
            number(args, sizeof(args), radix, code);
            break;

        case NONE:
            break;
    }

    snprintf(d->text, sizeof(d->text), "%s%s%s", name, *args ? " " : "", args);
}
