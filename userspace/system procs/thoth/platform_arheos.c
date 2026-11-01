/*
 * Copyright (C) 2026 ARHeOS by Dmitry H
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "include/thoth.h"
#include "include/thoth_platform.h"


// === Данный файл предназначен только для сборки под ARHEOS ===
#ifdef ARHEOS_OS

#include "../../include/ustd.h"
#include "../../include/umemory.h"
#include "../../../include-kernel/locale_types.h"
#include <stdint.h>
#include <stdbool.h>

//Состояния клавиатуры

static locale_keymap_t g_keymap;
static int      g_keymap_locale_idx = -2;
static bool     g_has_keymap = false;
static bool     g_shift = false, g_ctrl = false, g_alt = false, g_caps = false;
static bool     g_extended = false;

// Ctrl+letter: map scancode в lower-case Latin

static char sc_to_ctrl_letter(uint8_t sc){
    switch (sc){
        case 0x1E: return 'a'; case 0x30: return 'b'; case 0x2E: return 'c';
        case 0x20: return 'd'; case 0x12: return 'e'; case 0x21: return 'f';
        case 0x22: return 'g'; case 0x23: return 'h'; case 0x17: return 'i';
        case 0x24: return 'j'; case 0x25: return 'k'; case 0x26: return 'l';
        case 0x32: return 'm'; case 0x31: return 'n'; case 0x18: return 'o';
        case 0x19: return 'p'; case 0x10: return 'q'; case 0x13: return 'r';
        case 0x1F: return 's'; case 0x14: return 't'; case 0x16: return 'u';
        case 0x2F: return 'v'; case 0x11: return 'w'; case 0x2D: return 'x';
        case 0x15: return 'y'; case 0x2C: return 'z';
    }
    return 0;
}

//Жизненный цикл

int tp_init(void){
    u_kbd_set_mode(1);
    tp_poll_locale();
    return 0;
}

void tp_shutdown(void){
    u_kbd_set_mode(0);
}

void tp_poll_locale(void){
    int idx = u_locale_get();
    if (idx < 0) idx = 0;
    if (idx == g_keymap_locale_idx && g_has_keymap) return;
    if (u_locale_get_keymap(&g_keymap) == 0){
        g_keymap_locale_idx = idx;
        g_has_keymap = true;
    }
}

// I/O

int tp_write(int fd, const void *buf, uint32_t n){ return u_write(fd, buf, n); }

int tp_open(const char *path, uint32_t flags){
    uint32_t f = 0;
    if (flags & TP_OPEN_READ)     f |= FS_OPEN_READ;
    if (flags & TP_OPEN_WRITE)    f |= FS_OPEN_WRITE;
    if (flags & TP_OPEN_CREATE)   f |= FS_OPEN_CREATE;
    if (flags & TP_OPEN_TRUNCATE) f |= FS_OPEN_TRUNCATE;
    return open(path, f);
}
int tp_close(int fd){ return close(fd); }
int tp_seek(int fd, uint32_t pos){ return seek(fd, pos); }
int tp_read(int fd, void *buf, uint32_t n){ return u_read(fd, buf, n); }
int tp_unlink(const char *path){ return unlink(path); }
int tp_rename(const char *a, const char *b){ return rename(a, b); }

//Операции с консолью

void tp_clear_screen(void){ clear_screen(); }

void tp_geometry(uint32_t *cols, uint32_t *rows, uint32_t *cw, uint32_t *ch){
    uint32_t c = 8, r = 8;
    u_get_console_geometry(cols, rows, cw ? cw : &c, ch ? ch : &r);
}

//Ввод

bool tp_kbhit(void){ return u_kbhit() != 0; }

thoth_event_t tp_read_event(void){
    for (;;){
        uint32_t ev = u_read_scancode();
        if (ev == 0) { thoth_event_t e = {EV_NONE, 0, 0, false}; return e; }

        uint8_t sc = ev & 0xFF;
        uint8_t type = (ev >> 8) & 0xFF;

        if (sc == 0xE0){ if (type == 1) g_extended = true; continue; }

        if (type == 0){
            if (sc == 0x2A || sc == 0x36) g_shift = false;
            else if (sc == 0x1D)          g_ctrl  = false;
            else if (sc == 0x38)          g_alt   = false;
            else if (sc == 0x3A)          g_caps  = !g_caps;
            g_extended = false;
            continue;
        }

        if (g_extended){
            g_extended = false;
            thoth_event_t e = {EV_KEY, 0, 0, false};
            switch (sc){
                case 0x4B: e.key = TK_LEFT;  return e;
                case 0x4D: e.key = TK_RIGHT; return e;
                case 0x48: e.key = TK_UP;    return e;
                case 0x50: e.key = TK_DOWN;  return e;
                case 0x47: e.key = TK_HOME;  return e;
                case 0x4F: e.key = TK_END;   return e;
                case 0x49: e.key = TK_PGUP;  return e;
                case 0x51: e.key = TK_PGDN;  return e;
                case 0x53: e.key = TK_DEL;   return e;
                default: continue;
            }
        }

        if (sc == 0x2A || sc == 0x36){ g_shift = true; continue; }
        if (sc == 0x1D){ g_ctrl = true; continue; }
        if (sc == 0x38){ g_alt = true; continue; }

        if (sc == 0x01){ thoth_event_t e = {EV_KEY, 0, TK_ESC, false}; return e; }
        if (sc == 0x1C){ thoth_event_t e = {EV_KEY, 0, TK_ENTER, false}; return e; }
        if (sc == 0x0E){ thoth_event_t e = {EV_KEY, 0, TK_BACKSPACE, false}; return e; }
        if (sc == 0x0F){ thoth_event_t e = {EV_KEY, 0, TK_TAB, false}; return e; }

        if (sc >= 0x3B && sc <= 0x44){
            thoth_event_t e = {EV_KEY, 0, (thoth_key_t)(TK_F1 + (sc - 0x3B)), false};
            return e;
        }

        if (g_ctrl){
            char letter = sc_to_ctrl_letter(sc);
            if (letter){
                thoth_event_t e = {EV_CHAR, (uint32_t)letter, 0, true};
                return e;
            }
            continue;
        }

        if (!g_has_keymap || sc >= 128) continue;

        uint32_t cp = g_shift ? g_keymap.shift[sc] : g_keymap.normal[sc];
        if (g_caps){
            if (cp >= 'a' && cp <= 'z') cp = cp - 'a' + 'A';
            else if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
            else if (cp >= 0x430 && cp <= 0x44F) cp = cp - 0x430 + 0x410;
            else if (cp >= 0x410 && cp <= 0x42F) cp = cp - 0x410 + 0x430;
        }
        if (cp != 0){
            thoth_event_t e = {EV_CHAR, cp, 0, false};
            return e;
        }
    }
}

//Служебные вызовы

uint32_t tp_ticks(void){ return get_ticks(); }
void tp_yield(void){ yield(); }
bool tp_resized(void){ return false; }
void tp_exit(void){ u_exit(); for(;;); }

int      tp_clipboard_set(const void *b, uint32_t s) { return u_clipboard_set(b, s); }
uint32_t tp_clipboard_size(void)                     { return u_clipboard_size(); }
int      tp_clipboard_get(void *b, uint32_t m)       { return u_clipboard_get(b, m); }
void     tp_clipboard_clear(void)                    { u_clipboard_clear(); }

bool tp_take_paste_buf(const uint8_t **b, uint32_t *l){ (void)b; (void)l; return false; }

#endif //ARHEOS_OS
