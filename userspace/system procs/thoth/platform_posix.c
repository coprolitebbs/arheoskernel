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

#ifndef ARHEOS_OS

#ifdef _WIN32
#  include <windows.h>
#  include <conio.h>
#  include <io.h>
#  include <string.h>
#  define sleep_ms(ms) Sleep(ms)
   struct termios { DWORD mode; };
#else
#  include <termios.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/ioctl.h>
#  include <sys/select.h>
#  define sleep_ms(ms) do { struct timespec ts = {0, (ms) * 1000000}; nanosleep(&ts, NULL); } while(0)
#endif

#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
static DWORD g_saved_in_mode;
static DWORD g_saved_out_mode;
static UINT  g_saved_cp;
//Очередь для хранения сгенерированных событий под Windows
static thoth_event_t g_win_events[16];
static int           g_win_ev_head = 0;
static int           g_win_ev_tail = 0;
#else
static struct termios g_saved_tio;
static uint8_t        g_lookahead = 0;
static bool           g_lookahead_valid = false;

//Bracketed paste: один крупный блок, пришедший как ESC[200~ ... ESC[201~
static uint8_t        g_paste_buf[1024 * 1024];
static uint32_t       g_paste_pending = 0;
#endif

static volatile sig_atomic_t g_resized = 0;
static uint64_t       g_start_ms = 0;

bool tp_take_paste_buf(const uint8_t **buf, uint32_t *len){
#ifndef _WIN32
    if (g_paste_pending == 0) return false;
    if (buf) *buf = g_paste_buf;
    if (len) *len = g_paste_pending;
    g_paste_pending = 0;
    return true;
#else
    (void)buf; (void)len;
    return false;
#endif
}

static uint64_t now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

#ifndef _WIN32
static void on_signal(int sig){
    tp_shutdown();
    _exit(128 + sig);
}


static void on_sigwinch(int sig){
    (void)sig;
    g_resized = 1;
}
#endif

int tp_init(void){
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE) return -1;

    if (!GetConsoleMode(hIn, &g_saved_in_mode)) return -1;
    if (!GetConsoleMode(hOut, &g_saved_out_mode)) return -1;

    g_saved_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(65001); /* Принудительно ставим UTF-8 на вывод */

    DWORD rawIn = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
    DWORD rawOut = g_saved_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;

    SetConsoleMode(hIn, rawIn);
    SetConsoleMode(hOut, rawOut);

    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#else
    if (tcgetattr(0, &g_saved_tio) < 0) return -1;
    struct termios raw = g_saved_tio;
    cfmakeraw(&raw);
    if (tcsetattr(0, TCSAFLUSH, &raw) < 0) return -1;

    //Включаем Kitty keyboard protocol (progressive enhancement)
    //Поддерживающие терминалы начнут присылать Cmd/Ctrl+буква как CSI-u
    //Остальные молча проигнорируют последовательность
    {
        static const char kbd_on[] = "\x1B[>1u";
        tp_write(1, kbd_on, sizeof(kbd_on) - 1);
    }

    //Установить обработчики после перехода в raw-режим, чтобы не поймать SIGINT от Ctrl+C во время инициализации
    signal(SIGINT,   on_signal);
    signal(SIGTERM,  on_signal);
    signal(SIGHUP,   on_signal);
    signal(SIGWINCH, on_sigwinch);
#endif

    g_start_ms = now_ms();
    return 0;
}

void tp_shutdown(void){
#ifdef _WIN32
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_saved_in_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_saved_out_mode);
    SetConsoleOutputCP(g_saved_cp); // Возвращаем кодовую страницу
#else
    {
        static const char kbd_off[] = "\x1B[<u";
        tp_write(1, kbd_off, sizeof(kbd_off) - 1);
    }
    tcsetattr(0, TCSAFLUSH, &g_saved_tio);
    signal(SIGINT,   SIG_DFL);
    signal(SIGTERM,  SIG_DFL);
    signal(SIGHUP,   SIG_DFL);
    signal(SIGWINCH, SIG_DFL);
#endif
}

void tp_poll_locale(void) { }

int tp_write(int fd, const void *buf, uint32_t n){
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t left = n;
    while (left > 0){
        ssize_t r = write(fd, p, left);
        if (r < 0){ if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; left -= (uint32_t)r;
    }
    return (int)n;
}

int tp_open(const char *path, uint32_t flags){
    int f;
    if      ((flags & (TP_OPEN_READ|TP_OPEN_WRITE)) == (TP_OPEN_READ|TP_OPEN_WRITE)) f = O_RDWR;
    else if (flags & TP_OPEN_WRITE) f = O_WRONLY;
    else                            f = O_RDONLY;
    if (flags & TP_OPEN_CREATE)   f |= O_CREAT;
    if (flags & TP_OPEN_TRUNCATE) f |= O_TRUNC;
#ifdef _WIN32
    return open(path, f | O_BINARY, 0644);
#else
    return open(path, f, 0644);
#endif
}

int tp_close(int fd){ return close(fd); }
int tp_seek(int fd, uint32_t pos){ return (int)lseek(fd, (off_t)pos, SEEK_SET); }

int tp_read(int fd, void *buf, uint32_t n){
    for (;;){
        ssize_t r = read(fd, buf, n);
        if (r < 0){ if (errno == EINTR) continue; return -1; }
        return (int)r;
    }
}

int tp_unlink(const char *path){ return unlink(path); }

int tp_rename(const char *a, const char *b){
#ifdef _WIN32
    //Windows rename() падает, если целевой файл существует. Используем нативное API
    wchar_t wA[MAX_PATH];
    wchar_t wB[MAX_PATH];

    if (MultiByteToWideChar(CP_UTF8, 0, a, -1, wA, MAX_PATH) == 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, b, -1, wB, MAX_PATH) == 0) return -1;

    if (MoveFileExW(wA, wB, MOVEFILE_REPLACE_EXISTING)) {
        return 0;
    }
    return -1;
#else
    return rename(a, b);
#endif
}


void tp_clear_screen(void){
    const char *s = "\x1B[2J\x1B[H\x1B[?25h";
    tp_write(1, s, (uint32_t)strlen(s));
}

void tp_geometry(uint32_t *cols, uint32_t *rows, uint32_t *cw, uint32_t *ch){
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        if (cols) *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (rows) *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        if (cols) *cols = 80;
        if (rows) *rows = 24;
    }
#else
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0){
        if (cols) *cols = ws.ws_col;
        if (rows) *rows = ws.ws_row;
    } else {
        if (cols) *cols = 80;
        if (rows) *rows = 24;
    }
#endif
    if (cw) *cw = 1;
    if (ch) *ch = 1;
}

#ifdef _WIN32
//Вспомогательная функция добавления нативных Windows событий во внутреннее кольцо
static void push_win_event(thoth_event_t e) {
    int next = (g_win_ev_tail + 1) % 16;
    if (next != g_win_ev_head) {
        g_win_events[g_win_ev_tail] = e;
        g_win_ev_tail = next;
    }
}

//Нативный парсинг очереди событий Windows API
static void win32_read_all_available(void) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD numEvents = 0;
    if (!GetNumberOfConsoleInputEvents(hIn, &numEvents) || numEvents == 0) return;

    INPUT_RECORD records[32];
    DWORD readCount = 0;
    if (!ReadConsoleInputW(hIn, records, numEvents > 32 ? 32 : numEvents, &readCount)) return;

    for (DWORD i = 0; i < readCount; i++) {
        if (records[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
            g_resized = 1;
            continue;
        }
        if (records[i].EventType == KEY_EVENT && records[i].Event.KeyEvent.bKeyDown) {
            KEY_EVENT_RECORD k = records[i].Event.KeyEvent;
            thoth_event_t e = {EV_NONE, 0, 0, false, false};

            //Проверяем функциональные и управляющие клавиши по Virtual Key
            switch (k.wVirtualKeyCode) {
                case VK_ESCAPE: e.kind = EV_KEY; e.key = TK_ESC; push_win_event(e); continue;
                case VK_RETURN: e.kind = EV_KEY; e.key = TK_ENTER; push_win_event(e); continue;
                case VK_BACK:   e.kind = EV_KEY; e.key = TK_BACKSPACE; push_win_event(e); continue;
                case VK_TAB:    e.kind = EV_KEY; e.key = TK_TAB; push_win_event(e); continue;
                case VK_LEFT:   e.kind = EV_KEY; e.key = TK_LEFT; push_win_event(e); continue;
                case VK_RIGHT:  e.kind = EV_KEY; e.key = TK_RIGHT; push_win_event(e); continue;
                case VK_UP:     e.kind = EV_KEY; e.key = TK_UP; push_win_event(e); continue;
                case VK_DOWN:   e.kind = EV_KEY; e.key = TK_DOWN; push_win_event(e); continue;
                case VK_HOME:   e.kind = EV_KEY; e.key = TK_HOME; push_win_event(e); continue;
                case VK_END:    e.kind = EV_KEY; e.key = TK_END; push_win_event(e); continue;
                case VK_PRIOR:  e.kind = EV_KEY; e.key = TK_PGUP; push_win_event(e); continue;
                case VK_NEXT:   e.kind = EV_KEY; e.key = TK_PGDN; push_win_event(e); continue;
                case VK_DELETE: e.kind = EV_KEY; e.key = TK_DEL; push_win_event(e); continue;
                case VK_F1:     e.kind = EV_KEY; e.key = TK_F1; push_win_event(e); continue;
                case VK_F2:     e.kind = EV_KEY; e.key = TK_F2; push_win_event(e); continue;
                case VK_F3:     e.kind = EV_KEY; e.key = TK_F3; push_win_event(e); continue;
                case VK_F4:     e.kind = EV_KEY; e.key = TK_F4; push_win_event(e); continue;
                case VK_F5:     e.kind = EV_KEY; e.key = TK_F5; push_win_event(e); continue;
                case VK_F6:     e.kind = EV_KEY; e.key = TK_F6; push_win_event(e); continue;
                case VK_F7:     e.kind = EV_KEY; e.key = TK_F7; push_win_event(e); continue;
                case VK_F8:     e.kind = EV_KEY; e.key = TK_F8; push_win_event(e); continue;
                case VK_F9:     e.kind = EV_KEY; e.key = TK_F9; push_win_event(e); continue;
                case VK_F10:    e.kind = EV_KEY; e.key = TK_F10; push_win_event(e); continue;
                case VK_F11:    e.kind = EV_KEY; e.key = TK_F11; push_win_event(e); continue;
                case VK_F12:    e.kind = EV_KEY; e.key = TK_F12; push_win_event(e); continue;
            }

            //Обработка текстового ввода с поддержкой суррогатных пар Unicode
            wchar_t wc = k.uChar.UnicodeChar;
            if (wc != 0) {
                e.kind = EV_CHAR;
                e.cp = wc;
                e.ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

                //Если это Ctrl + буква (в Windows это управляющие коды 1-26), приводим к чистому символу
                if (e.ctrl && e.cp >= 1 && e.cp <= 26) {
                    e.cp = 'a' + (e.cp - 1);
                }
                push_win_event(e);
            }
        }
    }
}
#else
static bool try_get_byte(uint8_t *out, int timeout_ms){
    for (;;){
        fd_set rf; FD_ZERO(&rf); FD_SET(0, &rf);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = select(1, &rf, NULL, NULL, &tv);
        if (r < 0){ if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        ssize_t rd;
        do { rd = read(0, out, 1); } while (rd < 0 && errno == EINTR);
        return rd == 1;
    }
}

static int get_byte_blocking(uint8_t *out){
    if (g_lookahead_valid){ *out = g_lookahead; g_lookahead_valid = false; return 1; }
    for (;;){
        ssize_t r = read(0, out, 1);
        if (r < 0){ if (errno == EINTR) continue; return -1; }
        return (int)r;
    }
}
#endif

bool tp_kbhit(void){
#ifdef _WIN32
    if (g_win_ev_head != g_win_ev_tail) return true;
    win32_read_all_available();
    return g_win_ev_head != g_win_ev_tail;
#else
	if (g_lookahead_valid) return true;
	for (;;){
		fd_set rf; FD_ZERO(&rf); FD_SET(0, &rf);
		struct timeval tv = {0, 0};
		int r = select(1, &rf, NULL, NULL, &tv);
		if (r < 0 && errno == EINTR) continue;
		return r > 0;
	}
#endif
}

#ifndef _WIN32
static thoth_event_t ev_none(void){ thoth_event_t e = {EV_NONE, 0, 0, false, false}; return e; }

static thoth_event_t ev_key(thoth_key_t k){ thoth_event_t e = {EV_KEY, 0, k, false, false}; return e; }

static thoth_event_t ev_char(uint32_t cp, bool ctrl){ thoth_event_t e = {EV_CHAR, cp, 0, ctrl, false}; return e; }

static thoth_event_t parse_csi(void){
    uint8_t seq[16]; int n = 0;
    for (;;) {
        if (n >= 15) return ev_none();
        uint8_t b;
        if (!try_get_byte(&b, 50)) return ev_none();
        seq[n++] = b;
        if (b >= 0x40 && b <= 0x7E) break;
    }
    uint8_t final = seq[n-1];

    //--- Обработка CSI-u (Kitty keyboard protocol) ---
    if (final == 'u') {
        //Формат: ESC [ key ; mods u   (mods может отсутствовать)
        int i = 0;
        uint32_t key = 0, mods = 1;
        //читаем key
        while (i < n-1 && seq[i] >= '0' && seq[i] <= '9') {
            key = key * 10 + (seq[i] - '0');
            i++;
        }
        if (i < n-1 && seq[i] == ';') {
            i++;
            mods = 0;
            while (i < n-1 && seq[i] >= '0' && seq[i] <= '9') {
                mods = mods * 10 + (seq[i] - '0');
                i++;
            }
        }
        //mods: 1 + (shift=1, alt=2, ctrl=4, super=8)
        bool has_super = (mods > 0) && ((mods - 1) & 8);
        bool has_ctrl  = (mods > 0) && ((mods - 1) & 4);

        if (has_super || has_ctrl) {
            //Нас интересуют только Cmd/Ctrl + c, v, x (и, возможно, z, y)
            if (key == 'c' || key == 'C') {
                thoth_event_t e = {EV_CHAR, 'c', 0, true, has_super};
                return e;
            }
            if (key == 'v' || key == 'V') {
                thoth_event_t e = {EV_CHAR, 'v', 0, true, has_super};
                return e;
            }
            if (key == 'x' || key == 'X') {
                thoth_event_t e = {EV_CHAR, 'x', 0, true, has_super};
                return e;
            }
            if (key == 'z' || key == 'Z') {
                thoth_event_t e = {EV_CHAR, 'z', 0, true, has_super};
                return e;
            }
            if (key == 'y' || key == 'Y') {
                thoth_event_t e = {EV_CHAR, 'y', 0, true, has_super};
                return e;
            }
        }
        return ev_none();
    }
    //--- Конец CSI-u ---

    if (final == '~' && n >= 2) {
        uint32_t num = 0;
        for (int k = 0; k < n - 1 && seq[k] >= '0' && seq[k] <= '9'; k++)
            num = num * 10 + (seq[k] - '0');

        if (num == 200) {
            uint32_t plen = 0;
            for (;;) {
                uint8_t b;
                if (!try_get_byte(&b, 200)) break;
                if (b == 0x1B) {
                    uint8_t b2;
                    if (!try_get_byte(&b2, 50)) break;
                    if (b2 != '[') continue;
                    char tail[8]; int tn = 0;
                    while (tn < 7) {
                        uint8_t t;
                        if (!try_get_byte(&t, 50)) break;
                        tail[tn++] = (char)t;
                        if (t == '~') break;
                    }
                    if (tn >= 4 && tail[0]=='2' && tail[1]=='0' && tail[2]=='1' && tail[3]=='~')
                        break;
                    continue;
                }
                if (plen < sizeof(g_paste_buf)) g_paste_buf[plen++] = b;
            }
            g_paste_pending = plen;
            thoth_event_t e = {EV_CHAR, 0xFFFF0000u, 0, false, false};
            return e;
        }
        if (num == 201) return ev_none();       //Bracketed paste end
    }

    if (n == 1) {
        switch (final) {
            case 'A': return ev_key(TK_UP);
            case 'B': return ev_key(TK_DOWN);
            case 'C': return ev_key(TK_RIGHT);
            case 'D': return ev_key(TK_LEFT);
            case 'H': return ev_key(TK_HOME);
            case 'F': return ev_key(TK_END);
        }
        return ev_none();
    }
	uint32_t num = 0;
	int i = 0;
	while (i < n - 1 && seq[i] >= '0' && seq[i] <= '9') {
		num = num * 10 + (seq[i] - '0');
		i++;
	}
	if (final == '~'){
		switch (num){
			case 1:  return ev_key(TK_HOME);
			case 3:  return ev_key(TK_DEL);
			case 4:  return ev_key(TK_END);
			case 5:  return ev_key(TK_PGUP);
			case 6:  return ev_key(TK_PGDN);
			case 11: return ev_key(TK_F1);
			case 12: return ev_key(TK_F2);
			case 13: return ev_key(TK_F3);
			case 14: return ev_key(TK_F4);
			case 15: return ev_key(TK_F5);
			case 17: return ev_key(TK_F6);
			case 18: return ev_key(TK_F7);
			case 19: return ev_key(TK_F8);
			case 20: return ev_key(TK_F9);
			case 21: return ev_key(TK_F10);
			case 23: return ev_key(TK_F11);
			case 24: return ev_key(TK_F12);
		}
	}
	if (n == 2 && seq[0] == '[') {
		switch (final) {
			case 'A': return ev_key(TK_F1);
			case 'B': return ev_key(TK_F2);
			case 'C': return ev_key(TK_F3);
			case 'D': return ev_key(TK_F4);
		}
	}
	return ev_none();
}

static thoth_event_t parse_ss3(void){
	uint8_t b;
	if (!try_get_byte(&b, 50)) return ev_none();
	switch (b){
		case 'A': return ev_key(TK_UP);
		case 'B': return ev_key(TK_DOWN);
		case 'C': return ev_key(TK_RIGHT);
		case 'D': return ev_key(TK_LEFT);
		case 'H': return ev_key(TK_HOME);
		case 'F': return ev_key(TK_END);
		case 'P': return ev_key(TK_F1);
		case 'Q': return ev_key(TK_F2);
		case 'R': return ev_key(TK_F3);
		case 'S': return ev_key(TK_F4);
	}
	return ev_none();
}

static uint32_t read_utf8_cp(uint8_t lead){
	if (lead < 0x80) return lead;
	uint32_t need = 0, cp = 0;
	if      ((lead & 0xE0) == 0xC0){ need = 1; cp = lead & 0x1F; }
	else if ((lead & 0xF0) == 0xE0){ need = 2; cp = lead & 0x0F; }
	else if ((lead & 0xF8) == 0xF0){ need = 3; cp = lead & 0x07; }
	else return 0xFFFD;
	for (uint32_t i = 0; i < need; i++){
		uint8_t b;
		if (!try_get_byte(&b, 50)) return 0xFFFD;
		if ((b & 0xC0) != 0x80)     return 0xFFFD;
		cp = (cp << 6) | (b & 0x3F);
	}
	return cp;
}
#endif

thoth_event_t tp_read_event(void){
#ifdef _WIN32
	if (g_win_ev_head == g_win_ev_tail) {
		//Если очередь пуста, блокируем поток до появления нативных системных событий
		HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
		while (g_win_ev_head == g_win_ev_tail) {
			WaitForSingleObject(hIn, INFINITE);
			win32_read_all_available();
		}
	}
	thoth_event_t e = g_win_events[g_win_ev_head];
	g_win_ev_head = (g_win_ev_head + 1) % 16;
	return e;
#else
	uint8_t b;
	if (get_byte_blocking(&b) != 1) return ev_none();
	if (b == 0x1B){
		uint8_t next;
		if (!try_get_byte(&next, 50)) return ev_key(TK_ESC);
		if (next == '[')              return parse_csi();
		if (next == 'O')              return parse_ss3();
		g_lookahead = next;
		g_lookahead_valid = true;
		return ev_key(TK_ESC);
	}
	if (b == '\r' || b == '\n')      return ev_key(TK_ENTER);
	if (b == 0x7F || b == 0x08)      return ev_key(TK_BACKSPACE);
	if (b == 0x09)                   return ev_key(TK_TAB);
	if (b >= 0x01 && b <= 0x1A)      return ev_char('a' + (uint32_t)(b - 1), true);
	if (b < 0x80) return ev_char(b, false);
	return ev_char(read_utf8_cp(b), false);
#endif
}

uint32_t tp_ticks(void){
	return (uint32_t)((now_ms() - g_start_ms) / 10);
}

void tp_yield(void){
	sleep_ms(1);
}

bool tp_resized(void){
	if (g_resized){
		g_resized = 0;
		return true;
	}
	return false;
}

void tp_exit(void){
	tp_clear_screen();
	tp_shutdown();
	exit(0);
}

//Простой локальный буфер обмена. Не использует системный (OSC 52 / xclip) — для тестов редактора хватает с головой
//static char    *g_clip_data = NULL;
//static uint32_t g_clip_size = 0;
//static uint32_t g_clip_cap  = 0;

/*
int tp_clipboard_set(const void *buf, uint32_t size){
    if (size > g_clip_cap){
        uint32_t nc = g_clip_cap ? g_clip_cap : 256;
        while (nc < size) nc *= 2;
        char *nb = (char *)realloc(g_clip_data, nc);
        if (!nb) return -1;
        g_clip_data = nb;
        g_clip_cap  = nc;
    }
    if (size && buf) memcpy(g_clip_data, buf, size);
    g_clip_size = size;
    return 0;
}

uint32_t tp_clipboard_size(void){ return g_clip_size; }

int tp_clipboard_get(void *buf, uint32_t max){
    if (!buf || max == 0) return (int)g_clip_size;
    uint32_t copy = g_clip_size < max ? g_clip_size : max;
    if (copy) memcpy(buf, g_clip_data, copy);
    return (int)copy;
}

void tp_clipboard_clear(void){ g_clip_size = 0; }
*/

#ifdef _WIN32

int tp_clipboard_set(const void *buf, uint32_t size) {
    if (size == 0) return 0;
    if (!OpenClipboard(NULL)) return -1;
    EmptyClipboard();

    // UTF-8 → UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf, (int)size, NULL, 0);
    if (wlen <= 0) { CloseClipboard(); return -1; }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(WCHAR));
    if (!hMem) { CloseClipboard(); return -1; }

    WCHAR *pMem = (WCHAR*)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, (const char*)buf, (int)size, pMem, wlen);
    pMem[wlen] = L'\0';
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return -1;
    }
    CloseClipboard();
    return 0;
}

uint32_t tp_clipboard_size(void) {
    if (!OpenClipboard(NULL)) return 0;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return 0; }

    WCHAR *p = (WCHAR*)GlobalLock(hData);
    if (!p) { CloseClipboard(); return 0; }

    int wlen = (int)wcslen(p);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, p, wlen, NULL, 0, NULL, NULL);
    GlobalUnlock(hData);
    CloseClipboard();
    return (ulen > 0) ? (uint32_t)ulen : 0;
}

int tp_clipboard_get(void *buf, uint32_t max) {
    if (!buf || max == 0) return (int)tp_clipboard_size();

    if (!OpenClipboard(NULL)) return -1;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return -1; }

    WCHAR *p = (WCHAR*)GlobalLock(hData);
    if (!p) { CloseClipboard(); return -1; }

    int wlen = (int)wcslen(p);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, p, wlen, (char*)buf, (int)max, NULL, NULL);
    GlobalUnlock(hData);
    CloseClipboard();

    return (ulen > 0) ? ulen : 0;
}

void tp_clipboard_clear(void) {
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

#else  // POSIX (macOS / Linux)

static uint32_t g_clip_size_cache = 0;  // кэш размера для tp_clipboard_size

// Записать данные во внешнюю команду (pbcopy, wl-copy, xclip)
static int clipboard_write_cmd(const char *cmd, const void *buf, uint32_t size) {
    FILE *fp = popen(cmd, "w");
    if (!fp) return -1;
    if (size > 0) {
        if (fwrite(buf, 1, size, fp) != size) {
            pclose(fp);
            return -1;
        }
    }
    return (pclose(fp) == 0) ? 0 : -1;
}

// Прочитать данные из внешней команды (pbpaste, wl-paste, xclip -o)
static int clipboard_read_cmd(const char *cmd, void **out_buf, uint32_t *out_size) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    uint32_t cap = 4096, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { pclose(fp); return -1; }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += (uint32_t)n;
        if (len == cap) {
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return -1; }
            buf = nb;
        }
    }
    pclose(fp);

    *out_buf  = buf;
    *out_size = len;
    return 0;
}

// Определяем команды для текущей платформы
#if defined(__APPLE__)
  #define CLIP_SET_CMD  "pbcopy"
  #define CLIP_GET_CMD  "pbpaste"
#elif defined(__linux__)
  static const char *clip_set_cmd(void) {
      const char *session = getenv("XDG_SESSION_TYPE");
      if (session && strcmp(session, "wayland") == 0)
          return "wl-copy";
      return "xclip -selection clipboard";  // X11
  }
  static const char *clip_get_cmd(void) {
      const char *session = getenv("XDG_SESSION_TYPE");
      if (session && strcmp(session, "wayland") == 0)
          return "wl-paste";
      return "xclip -selection clipboard -o";
  }
#else
  // Другие POSIX-системы (например, FreeBSD) — можно использовать xclip
  #define CLIP_SET_CMD  "xclip -selection clipboard"
  #define CLIP_GET_CMD  "xclip -selection clipboard -o"
#endif

int tp_clipboard_set(const void *buf, uint32_t size) {
#if defined(__linux__)
    const char *cmd = clip_set_cmd();
#else
    const char *cmd = CLIP_SET_CMD;
#endif
    int r = clipboard_write_cmd(cmd, buf, size);
    if (r == 0) g_clip_size_cache = size;
    return r;
}

uint32_t tp_clipboard_size(void) {
    return g_clip_size_cache;
}

int tp_clipboard_get(void *buf, uint32_t max) {
    if (!buf || max == 0) return (int)g_clip_size_cache;

#if defined(__linux__)
    const char *cmd = clip_get_cmd();
#else
    const char *cmd = CLIP_GET_CMD;
#endif
    void *tmp = NULL;
    uint32_t sz = 0;
    if (clipboard_read_cmd(cmd, &tmp, &sz) != 0) return -1;

    uint32_t copy = (sz < max) ? sz : max;
    if (copy) memcpy(buf, tmp, copy);
    free(tmp);
    g_clip_size_cache = sz;
    return (int)copy;
}

void tp_clipboard_clear(void) {
    // Очистка через запись пустой строки
    tp_clipboard_set("", 0);
    g_clip_size_cache = 0;
}

#endif  // _WIN32 / POSIX





#endif // !ARHEOS_OS
