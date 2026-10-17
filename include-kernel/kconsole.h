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

#ifndef KCONSOLE_H
#define KCONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"

//Границы виртуального текстового окна вывода

#define CONSOLE_COLS     128  //1024 / 8
#define CONSOLE_ROWS     96   //768 / 8
#define LINE_HEIGHT      8    //Высота шрифта 8 пикселей

#define LINE_BUFFER_SIZE 1024

#define TTY_TERMINALS_COUNT 1
#define MAX_CURSOR_TICK_COUNT 40
#define DEFAULT_CURSOR_CODEPOINT  0x2588   //U+2588 FULL BLOCK

extern uint32_t text_cursor_x;
extern uint32_t text_cursor_y;

extern uint32_t console_bgcolor;
extern uint32_t console_fgcolor;

// Состояния конечного автомата ANSI парсера
typedef enum {
    ANSI_STATE_NORMAL = 0,
    ANSI_STATE_ESC,      //Поймали символ Escape (0x1B)
    ANSI_STATE_CSI       //Поймали управляющий символ '['
} ansi_state_t;

#define ANSI_BUF_SIZE 32

//Дефолтные системные цвета для сброса (\x1B[0m)
#define DEFAULT_FG_COLOR 0x00FFFFFF //Белый
#define DEFAULT_BG_COLOR 0x00000000 //Черный

typedef struct{
    uint32_t codepoint;   //Unicode codepoint, 0 = пусто
    uint32_t color;
} console_cell_t;

//Структура параметров и состояния виртуального терминала TTY
typedef struct tty {
    int id;                                     //Идентификатор терминала (0, 1, 2...)
    char line_buffer[LINE_BUFFER_SIZE];         //Локальный буфер ввода строки ядра
    volatile uint32_t copied_bytes;            //Сколько байт накоплено в line_buffer
    volatile bool line_ready;                   //Флаг готовности строки (нажат Enter)

    volatile uint32_t line_cursor_pos;          //Индекс курсора ВНУТРИ line_buffer (0 .. copied_bytes)
    volatile bool cursor_visible;               //Флаг фазы мигания: true = инверсный блок, false = обычный символ
    uint32_t last_blink_tick;                   //На каком тике таймера последний раз меняли фазу мигания
    uint32_t cursor_tick_count;                 //Максимальное количество тиков таймера для задержки мигания курсора
    //char cursor_code;                           //Дефолтный код курсорного блока в кодовой таблице
    uint32_t cursor_code;                       //Дефолтный код курсорного блока в кодовой таблице

    //Координаты начала ввода (отступ Shell)
    uint32_t input_start_x;
    uint32_t input_start_y;

    //Индивидуальные экранные координаты курсора для данного TTY
    uint32_t cursor_x;
    uint32_t cursor_y;

    //Ширина/высота консоли в знакоместах
    uint32_t console_cols;
    uint32_t console_rows;

    //Размер одной ячейки (берётся из шрифта активной локали)
    uint32_t cell_w;
    uint32_t cell_h;

    //Локальная матрица экрана для возможности переключения окон (виртуальных консолей)
    console_cell_t matrix[CONSOLE_ROWS][CONSOLE_COLS];

    //Раскладка консоли
    uint8_t active_layout;

    //PID процесса, владеющего вводом на этом терминале (foreground процесс)
    uint32_t foreground_pid;
} tty_t;


// Глобальные переменные подсистемы TTY
extern tty_t tty_terminals[TTY_TERMINALS_COUNT];
extern tty_t *current_tty;

void kconsole_get_render_coords(uint32_t buffer_idx, uint32_t *out_x, uint32_t *out_y);
void kconsole_update_cursor(void);
void kernel_stdout_write_codepoint(uint32_t cp, uint32_t color);
void kernel_stdout_write_char(char c, uint32_t color);
void kconsole_clear_current_line(void);

//Интерфейсы текстовой матрицы ядра
void kconsole_scroll_up(void);
void kconsole_redraw_screen(void);
uint32_t kconsole_get_char_at(uint32_t col, uint32_t row);

void kconsole_clear_screen(void);

void kprintf(const char *format, ...);

void kconsole_init(void);
void kconsole_reconfigure(cfg_file_t *cfg);

void kconsole_reset_utf8_state(void);

#endif
