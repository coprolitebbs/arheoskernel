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

#ifndef USTD_H
#define USTD_H

#include <stddef.h>
#include <stdint.h>
#include "fileattrs.h"
#include "../../include-kernel/locale_types.h"

#define MAX_PROCESS_FDS 16

#define STDIN  0
#define STDOUT 1
#define STDERR 2

#define O_NONBLOCK 0x0004 // Битовая маска флага неблокирующего режима

#define INPUT_MODE_RAW    0
#define INPUT_MODE_COOKED 1


//Копия структуры dirent из вашего ядра для Ring 3
typedef struct
{
    char name[256];
    uint32_t id;
    uint32_t size;
    uint32_t attributes;
    uint32_t inode;
    uint32_t type;
} u_dirent_t;

typedef struct {
    uint32_t size;
    uint32_t attributes;
    uint32_t create_time;
    uint32_t modify_time;
} u_stat_t;


//extern int main(void);
extern int main(int argc, char *argv[]);

//Обертки над системными вызовами ядра
void u_draw_char(uint32_t x, uint32_t y, char c, uint32_t color);
void u_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color);
void u_fill_screen(uint32_t color);
void yield(void);
void u_exit(void);
void* u_alloc_page(void);

//Наш самодельный printf
void u_printf(int x, int y, uint32_t color, const char *format, ...);

//Обертки для работы с файлами из Userspace
int open(const char *path, uint32_t flags);
int u_kbhit(void);
uint32_t u_read_scancode(void);
int u_read(int fd, void *buf, uint32_t count);
int u_write(int fd, const void *buf, uint32_t count);
int mkdir(const char *path);
int u_readdir(int fd, u_dirent_t *entry);
int close(int fd);
int unlink(const char *path);
int rmdir(const char *path);
void clear_screen(void);
void u_clear_tile(uint32_t x, uint32_t y, uint32_t color);
int u_spawn(const char *path, const char *cmdline);
int u_halt_os(void);
int u_wait_deity(int pid);
uint32_t get_ticks(void);
int chdir(const char *path);
int getcwd(char *buf, uint32_t max_len);
int stat(const char *path, u_stat_t *st);
int seek(int fd, uint32_t position);
int tell(int fd);
int set_wallpaper(const void *buffer, uint32_t size);
bool fileexists(const char *path);
bool directoryexists(const char *path);
int dup2(int oldfd, int newfd);
int u_kbd_set_mode(int mode);

uint32_t u_tty_get_flags(void);
void u_tty_set_flags(uint32_t new_flags, uint32_t mask);
int u_locale_get(void);
int u_locale_get_keymap(locale_keymap_t *out);
void u_get_console_geometry(uint32_t *cols, uint32_t *rows,uint32_t *cell_w, uint32_t *cell_h);
int  u_scroll_rect(int x, int y, int w, int h, int dy);
int  u_fb_blit(int x, int y, int w, int h, const void *src);

void printf(const char *format, ...);
int scanf(const char *format, ...);


#endif
