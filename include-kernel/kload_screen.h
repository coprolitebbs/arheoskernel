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

#ifndef KLOAD_SCREEN_H
#define KLOAD_SCREEN_H


//Ретро палитра в стиле  SGI / SunOS

#define C_BG_TOP      0x00283850u   //Steel blue top of the gradient
#define C_BG_BOTTOM   0x00142030u   //Dark slate bottom of the gradient
#define C_PANEL       0x00486080u   //Slate blue panel body
#define C_PANEL_IN    0x007890A8u   //Light slate inner bevel
#define C_PANEL_OUT   0x00B0C8E0u   //Silver-blue outer border
#define C_TITLE       0x00FFFFFFu   //Pure white title
#define C_TITLE_SHAD  0x001C2838u   //Dark slate title drop shadow
#define C_ACCENT      0x00FF6830u   //SGI orange (unchanged, canary)
#define C_ACCENT2     0x00FFB050u   //Lighter orange (pulse dots)
#define C_BAR_TRACK   0x00182838u   //Recessed empty bar track
#define C_BAR_EDGE    0x00A8B8D0u   //Light silver-blue bar border
#define C_BAR_FILL    0x0090C0F0u   //Sky-blue comet body
#define C_BAR_HOT     0x00FFC060u   //Hot orange comet tail
#define C_BAR_CORE    0x00FFFFFFu   //White comet head core
#define C_TEXT        0x00E8F0F8u   //Warm off-white body text
#define C_TEXT_BRIGHT 0x00FFFFFFu   //Bright white (headers)
#define C_TEXT_DIM    0x00A8B8C8u   //Dim grey (footer / subtitle)
#define C_STAR        0x008090A0u   //Dim star (now visible on slate)
#define C_STAR_BRIGHT 0x00E8F4FFu   //Bright star


//Настройки анимации
#define SPINNER_COUNT     4
#define BAR_SEGMENTS      30
#define COMET_HEAD_LEN    1
#define COMET_HOT_LEN     2
#define COMET_BODY_LEN    4
#define STAR_COUNT        44
#define PULSE_DOTS        3

//Ширина лога в символах (влезает в панель 480px с отступами 20+20)
#define LOG_COLS          54

//Размер кольцевого буфера в физических строках
//Больше, чем помещается на экране, чтобы при перерисовке после стирания
//фреймбуфера всё ещё был запас "истории" (хоть он и не виден)
#define LOG_CAPACITY      30

//Сколько строк лога помещается в отведённую область
#define LOG_VISIBLE       30    //6

//Межстрочный интервал в пикселях
#define LOG_LINE_H        14

//Отступ первой строки лога от заголовка "Boot Progress:"
#define LOG_FIRST_Y_OFF   22

extern bool load_screen_can_paint;

void kernel_load_animation_start(void);
void kernel_load_animation_next_step(void);
void kernel_load_animation_stop_nohide(void);
void kernel_load_animation_stop(void);
void load_screen_add_string(const char *fmt, ...);

#endif
