#ifndef MK90_DEBUGGER_H
#define MK90_DEBUGGER_H

#include "mk90.h"

enum { MK90_BREAKPOINTS = 32 };

typedef enum { MK90_VIEW_CPU,
               MK90_VIEW_RAM,
               MK90_VIEW_ROM
} mk90_memory_view;

typedef enum { MK90_DEBUG_RUNNING,
               MK90_DEBUG_PAUSE,
               MK90_DEBUG_BREAKPOINT,
               MK90_DEBUG_STEPS,
               MK90_DEBUG_CURSOR 
} mk90_stop_reason;

typedef struct {
    uint16_t address;
    bool     used, enabled;
} mk90_breakpoint;

typedef struct {
    bool             paused, skip_once, temporary;
    uint16_t         skip_address, temporary_address;
    uint32_t         remaining;
    mk90_stop_reason reason;
    mk90_breakpoint  breakpoints[MK90_BREAKPOINTS];
} mk90_debugger;

typedef struct {
    uint16_t address, next, words[3];
    bool     valid[3];
    unsigned count;
    char     text[128];
} mk90_disassembly;

/* Inspection never reads through the live I/O bus and never changes the machine. */

bool mk90_peek(const mk90 *m, uint16_t address, bool byte, mk90_memory_view view, uint16_t *value);
void mk90_disassemble(const mk90 *m, uint16_t address, unsigned radix, mk90_disassembly *out);
bool mk90_parse_number(const char *text, unsigned radix, uint32_t maximum, uint32_t *out);
void mk90_debug_init(mk90_debugger *d);
void mk90_debug_pause(mk90 *m, mk90_debugger *d);
void mk90_debug_continue(mk90 *m, mk90_debugger *d);
void mk90_debug_steps(mk90 *m, mk90_debugger *d, uint32_t count);
void mk90_debug_until(mk90 *m, mk90_debugger *d, uint16_t address);
void mk90_debug_run_us(mk90 *m, mk90_debugger *d, unsigned us);
int  mk90_debug_find(const mk90_debugger *d, uint16_t address);
int  mk90_debug_add(mk90_debugger *d, uint16_t address);

/* SMP import is transactional. Export does not clear the autosave dirty flag. */

bool mk90_smp_import(mk90 *m, unsigned slot, const char *path, char *error, size_t size);
bool mk90_smp_export(const mk90 *m, unsigned slot, const char *path, bool overwrite, char *error, size_t size);

#endif
