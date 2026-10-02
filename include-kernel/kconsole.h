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
#define DEFAULT_ASCII_CURSOR_CODE 0xDB

extern uint32_t text_cursor_x;
extern uint32_t text_cursor_y;

extern uint32_t console_bgcolor;
extern uint32_t console_fgcolor;

typedef struct{
    char ascii;
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
    char cursor_code;                           //Дефолтный код курсорного блока в кодовой таблице

    //Координаты начала ввода (отступ Shell)
    uint32_t input_start_x;
    uint32_t input_start_y;

    //Индивидуальные экранные координаты курсора для данного TTY
    uint32_t cursor_x;
    uint32_t cursor_y;

    //Локальная матрица экрана для возможности переключения окон (виртуальных консолей)
    console_cell_t matrix[CONSOLE_ROWS][CONSOLE_COLS];

    //PID процесса, владеющего вводом на этом терминале (foreground процесс)
    uint32_t foreground_pid;
} tty_t;


// Глобальные переменные подсистемы TTY
extern tty_t tty_terminals[TTY_TERMINALS_COUNT];
extern tty_t *current_tty;

extern console_cell_t console_matrix[CONSOLE_ROWS][CONSOLE_COLS];


void kconsole_get_render_coords(uint32_t buffer_idx, uint32_t *out_x, uint32_t *out_y);
void kconsole_update_cursor(void);
void kernel_stdout_write_char(char c, uint32_t color);
void kconsole_clear_current_line(void);

//Интерфейсы текстовой матрицы ядра
void kconsole_scroll_up(void);
void kconsole_redraw_screen(void);
char kconsole_get_char_at(uint32_t col, uint32_t row);

void kconsole_clear_screen(void);

void kprintf(const char *format, ...);

void kconsole_init(void);
void kconsole_reconfigure(cfg_file_t *cfg);

#endif
