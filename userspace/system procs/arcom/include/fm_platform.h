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

#ifndef FM_PLATFORM_H
#define FM_PLATFORM_H

#include "fm_types.h"

// -------- Файловая система --------
typedef struct fm_dir_handle fm_dir_handle_t;

fm_dir_handle_t *fm_opendir(const char *path);
int              fm_readdir(fm_dir_handle_t *dir, fm_dirent_t *out);
void             fm_closedir(fm_dir_handle_t *dir);

int  fm_stat(const char *path, fm_stat_t *st);
int  fm_mkdir(const char *path);
int  fm_rmdir(const char *path);
int  fm_unlink(const char *path);
int  fm_rename(const char *oldpath, const char *newpath);

int  fm_open(const char *path, uint32_t flags);
int  fm_read(int fd, void *buf, uint32_t size);
int  fm_write(int fd, const void *buf, uint32_t size);   // <- в файл
int  fm_close(int fd);

// -------- Вывод на консоль --------
void fm_console_write(const void *buf, uint32_t len);    // <- переименовано, в STDOUT
void fm_get_geometry(uint32_t *cols, uint32_t *rows);

// -------- Ввод с клавиатуры (блокирующий) --------
int  fm_read_key(void);

// -------- Процессы --------
int  fm_spawn(const char *path, const char *cmdline);
int  fm_wait(int pid);

// -------- Вспомогательное --------
void     fm_yield(void);
uint32_t fm_get_ticks(void);

// -------- Жизненный цикл --------
void fm_platform_init(void);
void fm_platform_shutdown(void);

#endif
