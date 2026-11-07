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

#ifndef FONT_VGA_H
#define FONT_VGA_H

#include <stdint.h>
#include <stddef.h>
#include "font.h"
#include "locale.h"
#include "font_data.h"

#define VGA_GLYPH_MAX 256

void font_vga_init(void);

void init_standard_locales(void);

//Конвертация CP866-байта в кодпойнт шрифта vga-8x8.
//Шрифт хранит глифы font_data[256][8] под Unicode-кодпойнтами:
//font_data[0xB1] (▒ в CP866) доступен под кодпойнтом U+2592.
//0x00..0x7F — ASCII, возвращается как есть; 0x80..0xFF — по таблице
//cp866_upper; значения больше 0xFF считаются уже готовым кодпойнтом
uint32_t cp866_byte_to_codepoint(uint32_t b);

#endif
