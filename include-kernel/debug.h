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

#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include <stddef.h>   // для size_t

extern unsigned int dbgp;

void draw_hex_dump(uint32_t addr, int num_bytes, int x, int y);
void draw_hex_byte(unsigned char byte, int x, int y);
void draw_hex_word(uint16_t word, int x, int y);
void draw_hex_dword(uint32_t dword, int x, int y);

void _test(void); //Точка стопа для LLDB

void draw_hex_dump_spaced(const void *addr, int num_bytes, int x, int y);
void draw_char_spaced(const void *addr, int num_bytes, int x, int y);

#endif
