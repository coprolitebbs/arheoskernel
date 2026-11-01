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

#ifndef THOTH_PLATFORM_H
#define THOTH_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

#define TP_OPEN_READ      0x0001
#define TP_OPEN_WRITE     0x0002
#define TP_OPEN_CREATE    0x0004
#define TP_OPEN_TRUNCATE  0x0010

#define TP_STDIN   0
#define TP_STDOUT  1


typedef enum {
    TK_ESC = 0,
    TK_ENTER,
    TK_BACKSPACE,
    TK_TAB,
    TK_LEFT, TK_RIGHT, TK_UP, TK_DOWN,
    TK_HOME, TK_END, TK_PGUP, TK_PGDN, TK_DEL,
    TK_F1, TK_F2, TK_F3, TK_F4, TK_F5, TK_F6,
    TK_F7, TK_F8, TK_F9, TK_F10, TK_F11, TK_F12,
} thoth_key_t;

typedef enum {
    EV_NONE = 0,
    EV_CHAR,
    EV_KEY,
} thoth_event_kind_t;

typedef struct {
    thoth_event_kind_t kind;
    uint32_t           cp;      //EV_CHAR: unicode codepoint
    thoth_key_t        key;     //EV_KEY
    bool               ctrl;    //EV_CHAR: Ctrl held
    bool               cmd;     //EV_CHAR: Command (macOS) / Super held
} thoth_event_t;

int  tp_init(void);
void tp_shutdown(void);

int  tp_write(int fd, const void *buf, uint32_t n);

int  tp_open (const char *path, uint32_t flags);
int  tp_close(int fd);
int  tp_seek (int fd, uint32_t pos);
int  tp_read (int fd, void *buf, uint32_t n);
int  tp_unlink(const char *path);
int  tp_rename(const char *oldpath, const char *newpath);

void tp_clear_screen(void);
void tp_geometry(uint32_t *cols, uint32_t *rows, uint32_t *cw, uint32_t *ch);

bool          tp_kbhit(void);
thoth_event_t tp_read_event(void);

//Периодический опрос: на ARHeOS проверяет смену раскладки, на POSIX — не обрабатывается
void tp_poll_locale(void);

uint32_t tp_ticks(void);   //100 Гц тики на обеих платформах
void     tp_yield(void);
void     tp_exit(void);

// true — если терминал изменил размер; флаг сбрасывается при чтении. На ARHeOS всегда false
bool tp_resized(void);

//Буфер обмена (Сессионный, для каждого TTY в ARHeOS; Или для локального процесса в POSIX)
int      tp_clipboard_set(const void *buf, uint32_t size);
uint32_t tp_clipboard_size(void);
int      tp_clipboard_get(void *buf, uint32_t max);
void     tp_clipboard_clear(void);

//Bracketed paste: если пришёл один крупный paste, вернуть указатель и длину
//Возвращает false, если забирать нечего. Указатель действителен до следующего вызова
bool tp_take_paste_buf(const uint8_t **buf, uint32_t *len);

#endif
