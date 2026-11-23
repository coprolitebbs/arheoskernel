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


#ifndef FM_TYPES_H
#define FM_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define FM_NAME_MAX    96
#define FM_PATH_MAX    512
#define FM_MAX_ENTRIES 256

typedef struct {
    char     name[FM_NAME_MAX];
    uint32_t size;
    uint8_t  attributes;
} fm_dirent_t;

typedef struct {
    uint32_t size;
    uint8_t  attributes;
} fm_stat_t;

#define FM_ATTR_DIRECTORY 0x01

// Численно совпадает с FS_OPEN_* из fileattrs.h (ARHeOS)
#define FM_OPEN_READ     0x01
#define FM_OPEN_WRITE    0x02
#define FM_OPEN_CREATE   0x04
#define FM_OPEN_APPEND   0x08
#define FM_OPEN_TRUNCATE 0x10

#define FM_OK    0
#define FM_ERR  -1

// Коды клавиш, платформонезависимые
#define FM_KEY_NONE      0x0000
#define FM_KEY_UP        0x0001
#define FM_KEY_DOWN      0x0002
#define FM_KEY_LEFT      0x0003
#define FM_KEY_RIGHT     0x0004
#define FM_KEY_HOME      0x0005
#define FM_KEY_END       0x0006
#define FM_KEY_PGUP      0x0007
#define FM_KEY_PGDN      0x0008
#define FM_KEY_DELETE    0x000A
#define FM_KEY_ENTER     0x000D
#define FM_KEY_TAB       0x0009
#define FM_KEY_ESC       0x001B
#define FM_KEY_BACKSPACE 0x007F

#define FM_KEY_CHAR(c)    (0x1000 | ((unsigned)(c) & 0xFF))
#define FM_KEY_IS_CHAR(k) (((k) & 0xF000) == 0x1000)
#define FM_KEY_GET_CHAR(k) ((k) & 0xFF)

#define FM_KEY_F1   0x0101
#define FM_KEY_F2   0x0102
#define FM_KEY_F3   0x0103
#define FM_KEY_F4   0x0104
#define FM_KEY_F5   0x0105
#define FM_KEY_F6   0x0106
#define FM_KEY_F7   0x0107
#define FM_KEY_F8   0x0108
#define FM_KEY_F9   0x0109
#define FM_KEY_F10  0x010A

#endif
