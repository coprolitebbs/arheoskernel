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

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include "../include/fm_types.h"
#include "../include/fm_platform.h"

// -------- Termios raw --------
static struct termios g_saved_termios;
static int g_termios_saved = 0;

void fm_platform_init(void)
{
    tcgetattr(0, &g_saved_termios);
    g_termios_saved = 1;

    struct termios raw = g_saved_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);   // тут TCSAFLUSH уместен: на входе мусор не нужен
}

void fm_platform_shutdown(void)
{
    if (g_termios_saved) tcsetattr(0, TCSAFLUSH, &g_saved_termios);
}

void fm_console_write(const void *buf, uint32_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(1, p, len);
        if (n <= 0) return;
        p += n;
        len -= (uint32_t)n;
    }
}

void fm_get_geometry(uint32_t *cols, uint32_t *rows)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80; *rows = 24;
    }
}

// -------- Ввод --------

// Читает 1 байт с таймаутом ~ms миллисекунд.
// ВАЖНО: восстанавливаем termios через TCSANOW, а НЕ TCSAFLUSH —
// иначе хвост escape-последовательности (стрелки, F-клавиши)
// сбрасывается прямо из TTY-очереди, и мы видим только одиночный ESC.
static int read_byte_timeout(unsigned char *out, int ms)
{
    struct termios orig, raw;
    tcgetattr(0, &orig);
    raw = orig;
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = (ms + 99) / 100;   // VTIME в 0.1с; для 30мс -> 1 (100мс)
    tcsetattr(0, TCSANOW, &raw);

    int r;
    do {
        errno = 0;
        r = (int)read(0, out, 1);
    } while (r < 0 && errno == EINTR);

    tcsetattr(0, TCSANOW, &orig);        // TCSANOW, не TCSAFLUSH — иначе теряем хвост ESC-последовательности
    return r > 0 ? 1 : 0;
}

static int errno_is_eintr(void)
{
    extern int errno;
    return errno == 4;   // EINTR
}

int fm_read_key(void)
{
    unsigned char c;
    for (;;) {
        if (read(0, &c, 1) != 1) continue;

        if (c == 0x1B) {
            unsigned char b1;
            if (!read_byte_timeout(&b1, 30)) return FM_KEY_ESC;
            if (b1 != '[' && b1 != 'O') return FM_KEY_ESC;

            unsigned char b2;
            if (!read_byte_timeout(&b2, 30)) return FM_KEY_ESC;

            // SS3 (application-mode): ESC O P/Q/R/S = F1..F4
            // Некоторые терминалы шлют и ESC O A/B/C/D для стрелок.
            if (b1 == 'O') {
                switch (b2) {
                case 'P': return FM_KEY_F1;
                case 'Q': return FM_KEY_F2;
                case 'R': return FM_KEY_F3;
                case 'S': return FM_KEY_F4;
                case 'A': return FM_KEY_UP;
                case 'B': return FM_KEY_DOWN;
                case 'C': return FM_KEY_RIGHT;
                case 'D': return FM_KEY_LEFT;
                case 'H': return FM_KEY_HOME;
                case 'F': return FM_KEY_END;
                }
                return FM_KEY_ESC;
            }

            // CSI letter
            switch (b2) {
            case 'A': return FM_KEY_UP;
            case 'B': return FM_KEY_DOWN;
            case 'C': return FM_KEY_RIGHT;
            case 'D': return FM_KEY_LEFT;
            case 'H': return FM_KEY_HOME;
            case 'F': return FM_KEY_END;
            case 'Z': return FM_KEY_ESC;   // Shift-Tab — игнорируем
            }

            // CSI digit...~ : и однозначные (ESC [ 5 ~), и двузначные (ESC [ 21 ~)
            if (b2 >= '0' && b2 <= '9') {
                unsigned char b3;
                if (!read_byte_timeout(&b3, 30)) return FM_KEY_ESC;
                int code;
                if (b3 == '~') {
                    code = b2 - '0';
                } else {
                    unsigned char b4;
                    if (!read_byte_timeout(&b4, 30)) return FM_KEY_ESC;
                    if (b4 != '~') return FM_KEY_ESC;
                    code = (b2 - '0') * 10 + (b3 - '0');
                }
                switch (code) {
                case 3:  return FM_KEY_DELETE;   // macOS Fn+Delete
                case 5:  return FM_KEY_PGUP;     // macOS PgUp
                case 6:  return FM_KEY_PGDN;     // macOS PgDn
                case 11: return FM_KEY_F1;
                case 12: return FM_KEY_F2;
                case 13: return FM_KEY_F3;
                case 14: return FM_KEY_F4;
                case 15: return FM_KEY_F5;
                case 17: return FM_KEY_F6;
                case 18: return FM_KEY_F7;
                case 19: return FM_KEY_F8;
                case 20: return FM_KEY_F9;
                case 21: return FM_KEY_F10;
                case 51: return FM_KEY_DELETE;   // Ctrl+Del в xterm
                case 53: return FM_KEY_PGUP;     // xterm PgUp
                case 54: return FM_KEY_PGDN;     // xterm PgDn
                }
            }
            return FM_KEY_ESC;
        }

        if (c == 0x7F || c == 0x08) return FM_KEY_BACKSPACE;
        if (c == '\r' || c == '\n') return FM_KEY_ENTER;
        if (c == '\t')              return FM_KEY_TAB;
        if (c >= 32 && c < 127)     return FM_KEY_CHAR((char)c);
    }
}

// -------- ФС --------

struct fm_dir_handle {
    DIR *d;
    char base[FM_PATH_MAX];
};

fm_dir_handle_t *fm_opendir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return NULL;
    fm_dir_handle_t *h = (fm_dir_handle_t *)malloc(sizeof(*h));
    if (!h) { closedir(d); return NULL; }
    h->d = d;
    strncpy(h->base, path, FM_PATH_MAX - 1);
    h->base[FM_PATH_MAX - 1] = 0;
    return h;
}

int fm_readdir(fm_dir_handle_t *h, fm_dirent_t *out)
{
    struct dirent *e;
    while ((e = readdir(h->d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        strncpy(out->name, e->d_name, FM_NAME_MAX - 1);
        out->name[FM_NAME_MAX - 1] = 0;

        char full[FM_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", h->base, e->d_name);
        if (n < 0 || n >= (int)sizeof(full)) continue;

        struct stat st;
        out->size       = 0;
        out->attributes = 0;
        if (lstat(full, &st) == 0) {
            out->size = (uint32_t)st.st_size;
            if (S_ISDIR(st.st_mode)) out->attributes |= FM_ATTR_DIRECTORY;
        }
        return FM_OK;
    }
    return FM_ERR;
}

void fm_closedir(fm_dir_handle_t *h)
{
    if (!h) return;
    closedir(h->d);
    free(h);
}

int fm_stat(const char *path, fm_stat_t *st)
{
    struct stat s;
    if (stat(path, &s) != 0) return FM_ERR;
    st->size       = (uint32_t)s.st_size;
    st->attributes = 0;
    if (S_ISDIR(s.st_mode)) st->attributes |= FM_ATTR_DIRECTORY;
    return FM_OK;
}

int fm_mkdir(const char *path)  { return mkdir(path, 0755) == 0 ? FM_OK : FM_ERR; }
int fm_rmdir(const char *path)  { return rmdir(path) == 0 ? FM_OK : FM_ERR; }
int fm_unlink(const char *path) { return unlink(path) == 0 ? FM_OK : FM_ERR; }
int fm_rename(const char *o, const char *n) { return rename(o, n) == 0 ? FM_OK : FM_ERR; }

int fm_open(const char *path, uint32_t flags)
{
    int f = 0;
    if (flags & FM_OPEN_READ)     f |= O_RDONLY;
    if (flags & FM_OPEN_WRITE)    f |= O_WRONLY;
    if (flags & FM_OPEN_CREATE)   f |= O_CREAT;
    if (flags & FM_OPEN_TRUNCATE) f |= O_TRUNC;
    return open(path, f, 0644);
}

int fm_read(int fd, void *buf, uint32_t size)         { return (int)read(fd, buf, size); }
int fm_write(int fd, const void *buf, uint32_t size)  { return (int)write(fd, buf, size); }
int fm_close(int fd)                                  { return close(fd); }

// -------- Процессы --------

int fm_spawn(const char *path, const char *cmdline)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char buf[512];
        strncpy(buf, cmdline, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;

        char *argv[32];
        int argc = 0;
        char *p = buf;
        while (*p && argc < 31) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
        argv[argc] = NULL;
        execv(path, argv);
        _exit(1);
    }
    return (int)pid;
}

int fm_wait(int pid)
{
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

void fm_yield(void) { }
uint32_t fm_get_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
