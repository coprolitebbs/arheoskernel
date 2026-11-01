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

#ifndef THOTH_H
#define THOTH_H

/*
 * Для сборки под ARHeOS — РАСКОММЕНТИРУЙТЕ строку ниже
 * Для сборки под Linux/macOS — ОСТАВЬТЕ закомментированной
 *
 * Платформенный слой выбирается сборкой:
 *   ARHeOS: build.sh компилирует platform_arheos.c
 *   POSIX (Linux/MacOS): Makefile компилирует platform_posix.c
 */
//#define ARHEOS_OS
//=========================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "thoth_platform.h"

#define MAX_FILE_SIZE  (0xFFFFFFFFu)
#define MAX_LINE_LEN   (64u * 1024u)

#define UNDO_MAX_DEPTH   32
#define UNDO_MAX_BYTES   (4u * 1024u * 1024u)   //суммарный бюджет на всю историю

#define CACHE_SIZE 512

//40 тиков по 100 Гц = 400 мс
#define BLINK_TICKS 40

//Типы фиксации выделения
typedef enum {
    SEL_NONE = 0,
    SEL_TRACKING,   //anchor зафиксирован, конец следует за курсором
    SEL_LOCKED      // anchor и конец заморожены
} sel_state_t;


typedef struct {
    uint32_t line_no;
    char    *text;
    uint32_t len;
    uint32_t cap;
} dirty_line_t;

typedef struct {
    uint32_t line_no;
    char    *text;
    uint32_t len;
    uint32_t cap;
    bool     valid;
} cache_slot_t;

typedef struct {
    uint32_t *line_offset;
    uint32_t  line_cap;
    uint32_t  line_count;
    dirty_line_t *dirty;
    uint32_t  dirty_count;
    uint32_t  dirty_cap;
    uint32_t  cursor_line;
    uint32_t  cursor_byte;
    uint32_t  scroll_top;
    uint32_t  hscroll;
    uint32_t  file_size;
    bool      is_new;
    uint32_t  mem_bytes;   //приблизительный размер снапшота
} editor_snapshot_t;

typedef struct {
    int      fd;
    char     filename[128];
    uint32_t file_size;
    bool     modified;
    bool     is_new;

    uint32_t *line_offset;
    uint32_t  line_count;
    uint32_t  line_cap;

    dirty_line_t *dirty;
    uint32_t      dirty_count;
    uint32_t      dirty_cap;

    cache_slot_t cache[CACHE_SIZE];

    uint32_t cursor_line;
    uint32_t cursor_byte;
    uint32_t scroll_top;
    uint32_t hscroll;

    uint32_t screen_rows, screen_cols;
    uint32_t edit_start, edit_rows;

    char     status[160];
    uint32_t status_until;
} editor_t;



void history_clear(void);
void sel_clear(void);

#endif
