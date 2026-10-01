#define _POSIX_C_SOURCE 200809L

#include "debug_ui.h"
#include "x11_host.h"
#include <X11/keysym.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum { ROWS = 11 };

static struct {
    mk90            *m;
    mk90_debugger   *debug;
    mk90_x11_window *window;
    lv_display_t    *display, *main_display;
    lv_group_t      *group;
    lv_obj_t        *status, *notice, *registers, *memory, *cards;
    lv_obj_t        *row[ROWS], *row_text[ROWS], *bp_table;
    lv_obj_t        *code_address, *mem_address, *bp_address, *count, *path, *radix_box, *view_box, *width_box, *slot_box;
    lv_obj_t        *confirm;
    unsigned         radix, confirm_slot;
    char             confirm_path[4096];
    bool             offscreen, closing, was_paused, follow_pc, control;
    int              confirmation;
    uint16_t         code_start, mem_start, cursor, last_pc, row_address[ROWS], next_page;
    unsigned         bp_index[MK90_BREAKPOINTS], bp_count;
    uint32_t         last_refresh;
} view;

enum { PAUSE,
       CONTINUE,
       STEP,
       STEPS,
       UNTIL,
       SHOW_PC,
       CODE_GO,
       CODE_PREV,
       CODE_NEXT,
       TOGGLE_CURSOR,
       MEM_GO,
       MEM_PREV,
       MEM_NEXT,
       BP_ADD,
       BP_TOGGLE,
       BP_DELETE,
       LOAD_CARD,
       SAVE_CARD,
       CLOSE,
       CONFIRM,
       CANCEL };

static void refresh(bool force);
static void action(lv_event_t *e);

static void notice(const char *text) {
    lv_label_set_text(view.notice, text);
}

static void fmt(char *buf, size_t size, uint16_t n) {
    snprintf(buf, size, view.radix == 16 ? "%04X" : "%06o", n);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width, bool mono) {
    lv_obj_t *o = lv_label_create(parent);

    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);

    if (width)
        lv_obj_set_width(o, width);

    if (mono) {
        lv_obj_set_style_text_font(o, &lv_font_unscii_8, 0);
        lv_obj_set_style_transform_pivot_y(o, 0, 0);
        lv_obj_set_style_transform_scale_y(o, 512, 0);
    }
    return o;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int id) {
    lv_obj_t *o = lv_button_create(parent);

    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 32);

    lv_obj_t *l = lv_label_create(o);

    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_group_add_obj(view.group, o);
    lv_obj_add_event_cb(o, action, LV_EVENT_CLICKED, (void *) (uintptr_t) id);

    return o;
}

static lv_obj_t *entry(lv_obj_t *parent, const char *text, int x, int y, int width) {
    lv_obj_t *o = lv_textarea_create(parent);

    lv_textarea_set_one_line(o, true);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, width, 34);
    lv_textarea_set_text(o, text);
    lv_textarea_set_text_selection(o, true);
    lv_group_add_obj(view.group, o);

    return o;
}

static void selection(lv_event_t *e) {
    (void) e;

    unsigned radix = lv_dropdown_get_selected(view.radix_box) ? 16 : 8;

    if (radix != view.radix) {
        /* Preserve entered addresses when changing radix. */
        lv_obj_t *fields[] = { view.code_address, view.mem_address, view.bp_address };

        for (unsigned i = 0; i < 3; i++) {
            uint32_t value;

            if (mk90_parse_number(lv_textarea_get_text(fields[i]), view.radix, 65535, &value)) {
                char buf[16];
                snprintf(buf, sizeof(buf), radix == 16 ? "%04X" : "%06o", value);
                lv_textarea_set_text(fields[i], buf);
            }
        }
        view.radix = radix;
    }
    refresh(true);
}

static lv_obj_t *dropdown(lv_obj_t *screen, const char *options, int x, int y, int w) {
    lv_obj_t *o = lv_dropdown_create(screen);

    lv_dropdown_set_options(o, options);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 34);
    lv_group_add_obj(view.group, o);
    lv_obj_add_event_cb(o, selection, LV_EVENT_VALUE_CHANGED, NULL);

    return o;
}

static bool address(lv_obj_t *field, uint16_t *out, bool aligned) {
    uint32_t value;

    if (!mk90_parse_number(lv_textarea_get_text(field), view.radix, 65535, &value) || (aligned && (value & 1))) {
        notice(aligned ? "Enter an even 16-bit address in the selected radix." : "Invalid address for the selected radix.");
        return false;
    }

    *out = (uint16_t) value;

    return true;
}

static void select_row(lv_event_t *e) {
    unsigned row = (unsigned) (uintptr_t) lv_event_get_user_data(e);

    view.cursor = view.row_address[row];
    view.follow_pc = false;

    refresh(true);
}

static void dismiss_confirmation(void) {
    if (view.confirm) {
        lv_obj_delete(view.confirm);
        view.confirm = NULL;
    }

    view.confirmation = 0;
}

static void ask_confirmation(int kind, unsigned slot, const char *path, const char *question) {
    view.confirmation = kind;
    view.confirm_slot = slot;

    snprintf(view.confirm_path, sizeof(view.confirm_path), "%s", path);

    view.confirm = lv_obj_create(lv_display_get_screen_active(view.display));

    lv_obj_set_size(view.confirm, 650, 160);
    lv_obj_center(view.confirm);
    lv_obj_set_style_bg_color(view.confirm, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_pad_all(view.confirm, 12, 0);
    lv_obj_set_style_text_color(view.confirm, lv_color_black(), 0);
    label(view.confirm, question, 8, 8, 595, false);

    lv_obj_t *yes = button(view.confirm, kind == 1 ? "Replace card" : "Overwrite file", 8, 85, 190, CONFIRM);

    button(view.confirm, "Cancel", 214, 85, 100, CANCEL);
    lv_group_focus_obj(yes);
}

static void card_operation(bool load, bool confirmed) {
    if (!view.debug->paused) {
        notice("Pause the CPU before changing or exporting a card.");
        return;
    }

    unsigned slot = confirmed ? view.confirm_slot : lv_dropdown_get_selected(view.slot_box);
    char     path[4096];

    snprintf(path, sizeof(path), "%s", confirmed ? view.confirm_path : lv_textarea_get_text(view.path));

    if (!*path) {
        notice("Enter the full path of a binary SMP image.");
        return;
    }

    if (load && !confirmed && view.m->smp[slot].dirty) {
        ask_confirmation(1, slot, path, "This card was loaded or modified in this session. Export it first to keep a separate copy. Replace it?");
        return;
    }

    struct stat st;

    if (!load && !confirmed && lstat(path, &st) == 0) {
        ask_confirmation(2, slot, path, "The destination already exists. Overwrite it with the current card contents?");
        return;
    }

    if (confirmed)
        dismiss_confirmation();

    char error[4352];

    bool ok = load ? mk90_smp_import(view.m, slot, path, error, sizeof(error)) : mk90_smp_export(view.m, slot, path, confirmed, error, sizeof(error));

    if (ok)
        notice(load ? "Image loaded; CPU remains paused. Reset MK-90 if the running program expects the previous card." : "Card exported. Export does not change the card's autosave state.");
    else
        notice(error);

    refresh(true);
}

static void perform(unsigned id) {
    if (view.confirm && id != CONFIRM && id != CANCEL && id != CLOSE)
        return;

    uint16_t       a;
    uint32_t       n;
    mk90_debugger *d = view.debug;

    switch (id) {
        case PAUSE:
            mk90_debug_pause(view.m, d);
            view.follow_pc = true;
            break;

        case CONTINUE:
            mk90_debug_continue(view.m, d);
            break;

        case STEP:
            mk90_debug_steps(view.m, d, 1);
            view.follow_pc = true;
            break;

        case STEPS:
            if (!mk90_parse_number(lv_textarea_get_text(view.count), 10, 100000000, &n) || !n) {
                notice("Step count must be a decimal number from 1 to 100000000.");
                return;
            }
            mk90_debug_steps(view.m, d, n);
            view.follow_pc = true;
            break;

        case UNTIL:
            mk90_debug_until(view.m, d, view.cursor);
            break;

        case SHOW_PC:
            view.follow_pc = true;
            view.code_start = view.cursor = view.m->r[7];
            break;

        case CODE_GO:
            if (!address(view.code_address, &a, true))
                return;
            view.code_start = view.cursor = a;
            view.follow_pc = false;
            break;

        case CODE_PREV:
            view.code_start -= 2 * ROWS;
            view.cursor = view.code_start;
            view.follow_pc = false;
            break;

        case CODE_NEXT:
            view.code_start = view.next_page;
            view.cursor = view.code_start;
            view.follow_pc = false;
            break;

        case TOGGLE_CURSOR: {
            int index = mk90_debug_find(d, view.cursor);

            if (index >= 0)
                d->breakpoints[index].enabled = !d->breakpoints[index].enabled;
            else if (mk90_debug_add(d, view.cursor) < 0)
                notice("All 32 breakpoint slots are in use.");
            break;
        }

        case MEM_GO:
            if (!address(view.mem_address, &a, false))
                return;
            view.mem_start = a;
            break;

        case MEM_PREV:
            view.mem_start -= 128;
            break;

        case MEM_NEXT:
            view.mem_start += 128;
            break;

        case BP_ADD:
            if (!address(view.bp_address, &a, true))
                return;
            if (mk90_debug_add(d, a) < 0)
                notice("All 32 breakpoint slots are in use.");
            break;

        case BP_TOGGLE:
        case BP_DELETE: {
            uint32_t row, col;
            lv_table_get_selected_cell(view.bp_table, &row, &col);

            if (row >= view.bp_count) {
                notice("Select a breakpoint first.");
                return;
            }

            mk90_breakpoint *bp = &d->breakpoints[view.bp_index[row]];

            if (id == BP_DELETE)
                bp->used = false;
            else
                bp->enabled = !bp->enabled;
            break;
        }

        case LOAD_CARD:
            card_operation(true, false);
            return;

        case SAVE_CARD:
            card_operation(false, false);
            return;

        case CONFIRM:
            card_operation(view.confirmation == 1, true);
            return;

        case CANCEL:
            dismiss_confirmation();
            return;

        case CLOSE:
            view.closing = true;
            return;
    }
    refresh(true);
}

static void action(lv_event_t *e) {
    perform((unsigned) (uintptr_t) lv_event_get_user_data(e));
}

static void keyboard(unsigned long key, bool down) {
    if (key == XK_Control_L || key == XK_Control_R) {
        view.control = down;
        return;
    }

    if (!down || view.closing)
        return;

    if (view.control) {
        lv_obj_t *focused = lv_group_get_focused(view.group);

        if ((key == XK_a || key == XK_A) && focused && lv_obj_check_type(focused, &lv_textarea_class)) {
            lv_textarea_set_cursor_pos(focused, LV_TEXTAREA_CURSOR_LAST);
            lv_obj_t *text = lv_textarea_get_label(focused);
            lv_label_set_text_selection_start(text, 0);
            lv_label_set_text_selection_end(text, lv_textarea_get_cursor_pos(focused));
        }
        return;
    }

    switch (key) {
        case XK_Escape:
            if (view.confirm)
                dismiss_confirmation();
            else
                view.closing = true;
            return;

        case XK_F3:
            perform(PAUSE);
            return;

        case XK_F5:
            perform(CONTINUE);
            return;

        case XK_F6:
            perform(STEP);
            return;

        case XK_F7:
            perform(STEPS);
            return;

        case XK_F8:
            perform(UNTIL);
            return;

        case XK_Pause:
            perform(PAUSE);
            return;

        case XK_Tab:
            lv_group_focus_next(view.group);
            return;

        case XK_ISO_Left_Tab:
            lv_group_focus_prev(view.group);
            return;

        case XK_Return:
        case XK_KP_Enter:
            key = LV_KEY_ENTER;
            break;

        case XK_BackSpace:
            key = LV_KEY_BACKSPACE;
            break;

        case XK_Delete:
            key = LV_KEY_DEL;
            break;

        case XK_Left:
            key = LV_KEY_LEFT;
            break;

        case XK_Right:
            key = LV_KEY_RIGHT;
            break;

        case XK_Up:
            key = LV_KEY_UP;
            break;

        case XK_Down:
            key = LV_KEY_DOWN;
            break;

        case XK_Home:
            key = LV_KEY_HOME;
            break;

        case XK_End:
            key = LV_KEY_END;
            break;

        default:
            if (key < 32 || key > 126)
                return;
            break;
    }

    lv_obj_t *focused = lv_group_get_focused(view.group);

    if (focused && lv_obj_check_type(focused, &lv_textarea_class)) {
        lv_obj_t *text = lv_textarea_get_label(focused);
        uint32_t  start = lv_label_get_text_selection_start(text);
        uint32_t  end = lv_label_get_text_selection_end(text);

        if (start != LV_LABEL_TEXT_SELECTION_OFF && end != LV_LABEL_TEXT_SELECTION_OFF && start < end) {
            bool erase = key == LV_KEY_BACKSPACE || key == LV_KEY_DEL;

            if (erase || (key >= 32 && key <= 126)) {
                lv_textarea_set_cursor_pos(focused, end);

                for (uint32_t i = start; i < end; i++)
                    lv_textarea_delete_char(focused);

                if (erase)
                    return;
            }
            lv_textarea_clear_selection(focused);
        }
    }
    lv_group_send_data(view.group, (uint32_t) key);
}

static void refresh(bool force) {
    if (!view.display || view.closing)
        return;

    uint32_t now = lv_tick_get();
    bool     paused = view.debug->paused;

    if (!force && paused == view.was_paused && now - view.last_refresh < 120)
        return;

    if (paused && !view.was_paused)
        view.follow_pc = true;

    view.was_paused = paused;
    view.last_refresh = now;

    if (view.follow_pc && (force || view.last_pc != view.m->r[7]))
        view.code_start = view.cursor = view.m->r[7];

    view.last_pc = view.m->r[7];

    static const char *reasons[] = { "Running", "Paused", "Breakpoint", "Step complete", "Run to cursor complete" };
    char               s[2048], value[16];

    fmt(value, sizeof(value), view.m->r[7]);
    snprintf(s, sizeof(s), "%s | PC %s | %s | %llu instructions | F5 continue, F6 step, F7 N steps, F8 run to cursor", reasons[view.debug->reason], value, view.m->wait ? "WAIT" : "CPU", (unsigned long long) view.m->instructions);
    lv_label_set_text(view.status, s);

    uint16_t a = view.code_start;

    for (unsigned i = 0; i < ROWS; i++) {
        mk90_disassembly line;

        mk90_disassemble(view.m, a, view.radix, &line);

        view.row_address[i] = a;
        int  bp = mk90_debug_find(view.debug, a);
        char words[24] = "", part[8];

        for (unsigned j = 0; j < 3; j++) {
            if (j < line.count) {
                if (line.valid[j])
                    fmt(part, sizeof(part), line.words[j]);
                else
                    snprintf(part, sizeof(part), view.radix == 16 ? "????" : "??????");
            } else
                snprintf(part, sizeof(part), view.radix == 16 ? "    " : "      ");

            strcat(words, part);
            strcat(words, " ");
        }

        fmt(value, sizeof(value), a);
        snprintf(s, sizeof(s), "%c%c %s  %s %s", a == view.m->r[7] ? '>' : ' ', bp < 0 ? ' ' : view.debug->breakpoints[bp].enabled ? '*'
                                                                                                                                   : '-',
                 value,
                 words,
                 line.text);

        lv_label_set_text(view.row_text[i], s);
        lv_obj_set_style_bg_color(view.row[i], lv_color_hex(a == view.m->r[7] ? 0x23563f : a == view.cursor ? 0x294e76
                                                                                                            : 0x111922),
                                  0);
        a = line.next;
    }

    view.next_page = a;
    s[0] = 0;

    for (unsigned i = 0; i < 8; i++) {
        char part[56];

        fmt(value, sizeof(value), view.m->r[i]);
        snprintf(part, sizeof(part), "%s%u %s%s", i == 6 ? "SP/R" : i == 7 ? "PC/R"
                                                                           : "   R",
                 i,
                 value,
                 i % 2 ? "\n" : "    ");
        strcat(s, part);
    }

    char part[512];

    fmt(value, sizeof(value), view.m->psw);
    snprintf(part, sizeof(part), "\nPSW %s\nH I T N Z V C\n%d %d %d %d %d %d %d\n\n", value, !!(view.m->psw & 256), !!(view.m->psw & 128), !!(view.m->psw & 16), !!(view.m->psw & 8), !!(view.m->psw & 4), !!(view.m->psw & 2), !!(view.m->psw & 1));
    strcat(s, part);

    char sys1[16], sys2[16];

    fmt(sys1, sizeof(sys1), view.m->sys1);
    fmt(sys2, sizeof(sys2), view.m->sys2);
    fmt(value, sizeof(value), view.m->lcd[0]);
    snprintf(part, sizeof(part), "RG1 %s  RG2 %s\nLCD %s  RAM %uK", sys1, sys2, value, view.m->ram_size / 1024);
    strcat(s, part);

    lv_label_set_text(view.registers, s);

    bool             bytes = lv_dropdown_get_selected(view.width_box) == 0;
    mk90_memory_view space = (mk90_memory_view) lv_dropdown_get_selected(view.view_box);

    s[0] = 0;

    for (unsigned row = 0; row < 8; row++) {
        a = view.mem_start + row * 16;
        fmt(value, sizeof(value), a);
        strcat(s, value);
        strcat(s, "  ");

        for (unsigned col = 0; col < 16; col += bytes ? 1 : 2) {
            uint16_t v;
            bool     valid = mk90_peek(view.m, a + col, bytes, space, &v);

            if (!valid)
                snprintf(part, sizeof(part), bytes ? (view.radix == 16 ? "?? " : "??? ") : (view.radix == 16 ? "???? " : "?????? "));
            else
                snprintf(part, sizeof(part), bytes ? (view.radix == 16 ? "%02X " : "%03o ") : (view.radix == 16 ? "%04X " : "%06o "), v);

            strcat(s, part);
        }
        strcat(s, "\n");
    }

    lv_label_set_text(view.memory, s);
    view.bp_count = 0;

    for (unsigned i = 0; i < MK90_BREAKPOINTS; i++)
        if (view.debug->breakpoints[i].used) {
            unsigned row = view.bp_count++;
            view.bp_index[row] = i;
            fmt(value, sizeof(value), view.debug->breakpoints[i].address);
            lv_table_set_cell_value(view.bp_table, row, 0, view.debug->breakpoints[i].enabled ? "on" : "off");
            lv_table_set_cell_value(view.bp_table, row, 1, value);
        }

    lv_table_set_row_count(view.bp_table, view.bp_count ? view.bp_count : 1);

    if (!view.bp_count) {
        lv_table_set_cell_value(view.bp_table, 0, 0, "-");
        lv_table_set_cell_value(view.bp_table, 0, 1, "No breakpoints");
    }

    snprintf(s, sizeof(s), "SMP0: %zu bytes, %s%s     |     SMP1: %zu bytes, %s%s", view.m->smp[0].size, view.m->smp[0].size >= 65536 ? "ROM" : "RAM", view.m->smp[0].dirty ? ", changed" : "", view.m->smp[1].size, view.m->smp[1].size >= 65536 ? "ROM" : "RAM", view.m->smp[1].dirty ? ", changed" : "");
    lv_label_set_text(view.cards, s);
}

void mk90_debug_ui_init(mk90 *m, mk90_debugger *debug, lv_display_t *main_display, bool offscreen) {
    memset(&view, 0, sizeof(view));

    view.m = m;
    view.debug = debug;
    view.main_display = main_display;
    view.offscreen = offscreen;
    view.radix = 8;
}

static void discard(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    (void) a;
    (void) p;

    lv_display_flush_ready(d);
}

void mk90_debug_ui_open(void) {
    if (!view.debug->paused)
        mk90_debug_pause(view.m, view.debug);

    if (view.display) {
        view.follow_pc = true;
        refresh(true);
        return;
    }

    static uint8_t buffer[1120 * 32 * 4];

    if (view.offscreen) {
        view.display = lv_display_create(1120, 760);
        lv_display_set_buffers(view.display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(view.display, discard);
    } else {
        view.window = mk90_x11_create("MK-90 Debugger", 1120, 760, keyboard, true);
        view.display = mk90_x11_display(view.window);
    }

    if (!view.display)
        return;

    view.closing = false;
    view.control = false;
    view.follow_pc = true;
    view.was_paused = true;
    view.code_start = view.cursor = view.m->r[7];
    view.group = lv_group_create();

    lv_display_set_default(view.display);

    lv_obj_t *screen = lv_display_get_screen_active(view.display);

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x18212b), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe5edf5), 0);
    button(screen, "Pause", 12, 12, 82, PAUSE);
    button(screen, "Continue", 102, 12, 104, CONTINUE);
    button(screen, "Step", 214, 12, 70, STEP);

    view.count = entry(screen, "1", 292, 12, 90);

    button(screen, "N steps", 390, 12, 92, STEPS);
    button(screen, "Run to cursor", 490, 12, 150, UNTIL);
    label(screen, "N: decimal", 654, 20, 112, false);

    view.radix_box = dropdown(screen, "Octal (8)\nHex (16)", 890, 12, 130);

    lv_dropdown_set_selected(view.radix_box, view.radix == 16);
    button(screen, "Close", 1028, 12, 80, CLOSE);

    view.status = label(screen, "", 12, 56, 1096, false);

    label(screen, "Code", 12, 88, 60, false);

    view.code_address = entry(screen, "000000", 70, 80, 110);

    button(screen, "Go", 188, 80, 54, CODE_GO);
    button(screen, "-22", 250, 80, 58, CODE_PREV);
    button(screen, "Next", 316, 80, 60, CODE_NEXT);
    button(screen, "PC", 384, 80, 52, SHOW_PC);
    button(screen, "Toggle BP at cursor", 444, 80, 214, TOGGLE_CURSOR);

    for (unsigned i = 0; i < ROWS; i++) {
        view.row[i] = lv_obj_create(screen);

        lv_obj_remove_style_all(view.row[i]);
        lv_obj_set_pos(view.row[i], 12, 120 + i * 22);
        lv_obj_set_size(view.row[i], 714, 22);
        lv_obj_set_style_bg_opa(view.row[i], LV_OPA_COVER, 0);
        lv_obj_set_scrollable(view.row[i], false);
        view.row_text[i] = label(view.row[i], "", 4, 3, 704, true);
        lv_label_set_long_mode(view.row_text[i], LV_LABEL_LONG_CLIP);
        lv_obj_add_event_cb(view.row[i], select_row, LV_EVENT_CLICKED, (void *) (uintptr_t) i);
    }

    label(screen, "Registers / flags (read only)", 744, 88, 360, false);
    view.registers = label(screen, "", 744, 120, 364, true);
    label(screen, "Memory", 12, 391, 74, false);
    view.mem_address = entry(screen, "000000", 86, 382, 100);
    button(screen, "Go", 194, 382, 54, MEM_GO);
    button(screen, "-128", 256, 382, 60, MEM_PREV);
    button(screen, "+128", 324, 382, 60, MEM_NEXT);
    view.view_box = dropdown(screen, "CPU map\nPhysical RAM\nPhysical ROM", 398, 382, 158);
    view.width_box = dropdown(screen, "Bytes\nWords", 566, 382, 130);
    view.memory = label(screen, "", 12, 430, 714, true);
    label(screen, "Breakpoints: stop BEFORE instruction", 744, 385, 364, false);
    view.bp_table = lv_table_create(screen);

    lv_obj_set_pos(view.bp_table, 744, 413);
    lv_obj_set_size(view.bp_table, 364, 116);
    lv_table_set_column_count(view.bp_table, 2);
    lv_table_set_column_width(view.bp_table, 0, 60);
    lv_table_set_column_width(view.bp_table, 1, 280);
    lv_obj_set_style_pad_ver(view.bp_table, 3, LV_PART_ITEMS);
    lv_group_add_obj(view.group, view.bp_table);

    view.bp_address = entry(screen, "000000", 744, 541, 100);

    button(screen, "Add", 852, 541, 60, BP_ADD);
    button(screen, "On/off", 920, 541, 86, BP_TOGGLE);
    button(screen, "Delete", 1014, 541, 94, BP_DELETE);
    label(screen, "SMP images - absolute path, load/export while paused", 12, 607, 1000, false);

    view.slot_box = dropdown(screen, "SMP0\nSMP1", 12, 634, 96);
    view.path = entry(screen, "", 116, 634, 774);

    lv_textarea_set_max_length(view.path, 4095);
    lv_textarea_set_placeholder_text(view.path, "/path/to/card.bin");
    button(screen, "Load", 900, 634, 96, LOAD_CARD);
    button(screen, "Export", 1004, 634, 104, SAVE_CARD);

    view.cards = label(screen, "", 12, 681, 1096, false);
    view.notice = label(screen, "Memory and registers are read-only in this version. Closing the debugger keeps the current run/pause state.", 12, 715, 1096, false);

    char text[16];

    fmt(text, sizeof(text), view.code_start);
    lv_textarea_set_text(view.code_address, text);
    refresh(true);
    lv_display_set_default(view.main_display);
}

bool mk90_debug_ui_visible(void) {
    return view.display != NULL;
}

void mk90_debug_ui_update(void) {
    refresh(false);
}

void mk90_debug_ui_poll(void) {
    if (!view.display)
        return;

    if ((!view.offscreen && !mk90_x11_poll_window(view.window)) || view.closing)
        mk90_debug_ui_close();
}

void mk90_debug_ui_close(void) {
    if (!view.display)
        return;

    view.closing = true;

    if (view.offscreen)
        lv_display_delete(view.display);
    else
        mk90_x11_destroy(view.window);

    view.display = NULL;
    view.window = NULL;
    view.confirm = NULL;
    view.confirmation = 0;

    lv_group_delete(view.group);
    view.group = NULL;
    lv_display_set_default(view.main_display);
}

bool mk90_debug_ui_snapshot(const char *path) {
    if (!view.display)
        return false;

    lv_obj_t *screen = lv_display_get_screen_active(view.display);
    lv_obj_update_layout(screen);
    lv_draw_buf_t *buf = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);

    if (!buf)
        return false;

    FILE *f = fopen(path, "wb");
    bool  ok = f != NULL;

    if (f) {
        ok = fprintf(f, "P6\n%u %u\n255\n", buf->header.w, buf->header.h) > 0;

        for (unsigned y = 0; y < buf->header.h; y++)
            for (unsigned x = 0; x < buf->header.w; x++) {
                const uint8_t *p = buf->data + y * buf->header.stride + x * 4;
                uint8_t        rgb[] = { p[2], p[1], p[0] };

                if (fwrite(rgb, 1, 3, f) != 3)
                    ok = false;
            }
        if (fclose(f))
            ok = false;
    }
    lv_draw_buf_destroy(buf);
    return ok;
}
