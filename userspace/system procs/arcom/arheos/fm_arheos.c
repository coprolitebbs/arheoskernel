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

#include <stdint.h>
#if defined(ARHEOS_OS)
#include "../../../../include-kernel/lib.h"
#else
#include <string.h>
#endif

#include "../include/fm_types.h"
#include "../include/fm_platform.h"

// ARHeOS userspace API
#include "../../../../userspace/include/ustd.h"
#include "../../../../userspace/include/umemory.h"
#include "../../../../userspace/include/syscall.h"
#include "../../../../include-kernel/locale_types.h"

// -------- Клавиатура --------

static int shift_pressed = 0;
static int extended      = 0;
static locale_keymap_t g_kmap;
static int g_kmap_ready = 0;

static void ensure_keymap(void)
{
    if (g_kmap_ready) return;
    if (u_locale_get_keymap(&g_kmap) == 0) g_kmap_ready = 1;
}

int fm_read_key(void)
{
    ensure_keymap();

    for (;;) {
        if (!u_kbhit()) { yield(); continue; }

        uint32_t ev = u_read_scancode();
        if (ev == 0) { yield(); continue; }

        uint8_t sc  = ev & 0xFF;
        uint8_t typ = (ev >> 8) & 0xFF;

        if (sc == 0x2A || sc == 0x36) {
            if (typ == 1) shift_pressed = 1;
            if (typ == 0) shift_pressed = 0;
            continue;
        }
        if (sc == 0xE0) {
            if (typ == 1) extended = 1;
            continue;
        }
        if (typ == 0) { extended = 0; continue; }

        if (extended) {
            extended = 0;
            switch (sc) {
            case 0x48: return FM_KEY_UP;
            case 0x50: return FM_KEY_DOWN;
            case 0x4B: return FM_KEY_LEFT;
            case 0x4D: return FM_KEY_RIGHT;
            case 0x47: return FM_KEY_HOME;
            case 0x4F: return FM_KEY_END;
            case 0x49: return FM_KEY_PGUP;
            case 0x51: return FM_KEY_PGDN;
            case 0x53: return FM_KEY_DELETE;
            }
            continue;
        }

        switch (sc) {
        case 0x01: return FM_KEY_ESC;
        case 0x0E: return FM_KEY_BACKSPACE;
        case 0x0F: return FM_KEY_TAB;
        case 0x1C: return FM_KEY_ENTER;
        case 0x3B: return FM_KEY_F1;
        case 0x3C: return FM_KEY_F2;
        case 0x3D: return FM_KEY_F3;
        case 0x3E: return FM_KEY_F4;
        case 0x3F: return FM_KEY_F5;
        case 0x40: return FM_KEY_F6;
        case 0x41: return FM_KEY_F7;
        case 0x42: return FM_KEY_F8;
        case 0x43: return FM_KEY_F9;
        case 0x44: return FM_KEY_F10;
        }

        if (sc < 128) {
            uint32_t cp = shift_pressed ? g_kmap.shift[sc] : g_kmap.normal[sc];
            if (cp >= 32 && cp < 127) return FM_KEY_CHAR((char)cp);
        }
    }
}

// -------- Вывод --------

void fm_console_write(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        uint32_t chunk = len > 4096 ? 4096 : len;
        u_write(1, p, chunk);
        p += chunk;
        len -= chunk;
    }
}

void fm_get_geometry(uint32_t *cols, uint32_t *rows)
{
    uint32_t c = 0, r = 0;
    u_get_console_geometry(&c, &r, 0, 0);
    if (c == 0) c = 80;
    if (r == 0) r = 25;
    *cols = c;
    *rows = r;
}

// -------- Файловая система --------

struct fm_dir_handle {
    int fd;
    char base[FM_PATH_MAX];
};

fm_dir_handle_t *fm_opendir(const char *path)
{
    int fd = open(path, FS_OPEN_READ);
    if (fd < 0) return NULL;

    fm_dir_handle_t *h = (fm_dir_handle_t *)malloc(sizeof(*h));
    if (!h) { close(fd); return NULL; }
    h->fd = fd;
    int i = 0;
    while (path[i] && i < FM_PATH_MAX - 1) { h->base[i] = path[i]; i++; }
    h->base[i] = 0;
    return h;
}

int fm_readdir(fm_dir_handle_t *h, fm_dirent_t *out)
{
    u_dirent_t e;
    int r = u_readdir(h->fd, &e);
    if (r != 0) return FM_ERR;

    int i = 0;
    while (e.name[i] && i < FM_NAME_MAX - 1) { out->name[i] = e.name[i]; i++; }
    out->name[i] = 0;
    out->size       = e.size;
    out->attributes = (uint8_t)e.attributes;
    return FM_OK;
}

void fm_closedir(fm_dir_handle_t *h)
{
    if (!h) return;
    close(h->fd);
    free(h);
}

int fm_stat(const char *path, fm_stat_t *st)
{
    u_stat_t u;
    if (stat(path, &u) != 0) return FM_ERR;
    st->size       = u.size;
    st->attributes = (uint8_t)u.attributes;
    return FM_OK;
}

int fm_mkdir(const char *path)  { return mkdir(path); }
int fm_rmdir(const char *path)  { return rmdir(path); }
int fm_unlink(const char *path) { return unlink(path); }

int fm_rename(const char *oldp, const char *newp)
{
    return rename(oldp, newp);
}

int fm_open(const char *path, uint32_t flags)
{
    return open(path, flags);   // флаги ARHeOS и FM совпадают численно
}

int fm_read(int fd, void *buf, uint32_t size)               { return u_read(fd, buf, size); }
int fm_write(int fd, const void *buf, uint32_t size)        { return u_write(fd, buf, size); }
int fm_close(int fd)                                        { return close(fd); }

// -------- Процессы --------

int fm_spawn(const char *path, const char *cmdline) { return u_spawn(path, cmdline); }
int fm_wait(int pid)                                { return u_wait_deity(pid); }

// -------- Misc --------

void     fm_yield(void)     { yield(); }
uint32_t fm_get_ticks(void) { return get_ticks(); }

void fm_platform_init(void)
{
    u_tty_set_flags(TTY_KBD_RAW, TTY_KBD_RAW);
}

void fm_platform_shutdown(void)
{
    u_tty_set_flags(TTY_ECHO, TTY_ECHO | TTY_KBD_RAW);
}
