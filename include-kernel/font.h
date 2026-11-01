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

#ifndef FONT_H
#define FONT_H

#include <stdint.h>

//Один глиф. Бимонарный образ лежит как height строк по ceil(width/8) байт,
//MSB — левый пиксель. Для моноширинных шрифтов 8x8 это ровно 8 байт
typedef struct {
    uint32_t       codepoint;   //U+0041, U+0416, U+2588, ...
    uint8_t        width;       //пикселей, обычно 8
    uint8_t        height;      //пикселей, обычно 8 или 16
    const uint8_t *rows;        //height строк данных
} font_glyph_t;

typedef struct {
    const char *name;           //"vga-8x8", "terminus-8x16"

    uint8_t     cell_width;     //ширина моноширинной ячейки
    uint8_t     cell_height;    //высота ячейки
    uint8_t     bytes_per_row;  //(cell_width + 7) / 8, кэш для скорости

    const font_glyph_t *glyphs; //массив, отсортирован по codepoint
    uint32_t            glyph_count;

    const font_glyph_t *fallback; //обычно указатель на глиф ■ (U+25A0)
} font_t;

//Линейный поиск (для 300-400 глифов быстрее и проще бинарного)
//Возвращает fallback, если codepoint не найден. Никогда не NULL.
const font_glyph_t *font_lookup(const font_t *f, uint32_t codepoint);

#endif
