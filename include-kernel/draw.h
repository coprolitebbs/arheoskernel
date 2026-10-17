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

#ifndef DRAW_H
#define DRAW_H
#include <stdint.h>

//Глобальные параметры VBE (определены в kernel.c, доступны через extern)
extern unsigned int lfb_addr;
extern unsigned int pitch;
extern unsigned int bytes_pp;

//Графические функции
void draw_char(unsigned int x, unsigned int y, unsigned char c);
void draw_string(unsigned int x, unsigned int y, const char *str);
void fill_screen(unsigned char r, unsigned char g, unsigned char b);

void kernel_draw_char(uint32_t x,uint32_t y,unsigned char c,uint32_t color);
void kernel_fill_screen(uint32_t color);
void kernel_draw_string(uint32_t x,uint32_t y,const char *str,uint32_t color);


#endif
