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

#ifndef KBD_INTERNAL_H
#define KBD_INTERNAL_H

#include "../../../include-kernel/bootinfo.h"

#include <stdint.h>
#include <stdbool.h>

//Аппаратные порты контроллера клавиатуры i8042 (ностальгический IBM PC/AT)
#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64

#define KBD_BUFFER_SIZE  256

static const char kbd_ascii_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,  '*',   0, ' ',
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,  '-',   0,   0,   0,  '+',   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0
};

#endif

