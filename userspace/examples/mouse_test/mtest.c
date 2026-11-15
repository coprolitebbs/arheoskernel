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

//mtest.c — тест драйвера мыши ARHeOS.
//Рисует курсор '+' по данным SYS_MOUSE_POLL, показывает позицию, дельту
//с прошлого опроса, кнопки и число устройств в реестре драйвера.
//Выход — любая клавиша (не раньше ~1.5 с после старта). Сборка:
//u_std_elf.o + u_memory_elf.o + u_mouse_elf.o + u_mtest.o + u_ugraph_elf.o.

#include "../../include/ustd.h"
#include "../../include/mouse.h"
#include "../../include/syscall.h"
#include <stdint.h>

#define COL_TEXT   0x00FFFFFF
#define COL_OK     0x0000FF00
#define COL_LEFT   0x000000FF   //в формате ядра (см. bgcolor в boot.cfg)
#define COL_RIGHT  0x00FF0000
#define COL_MID    0x0000FFFF
#define COL_BG     0x00000000

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t cols, rows, cw, ch;
    int last_px = -1, last_py = -1;
    u_mouse_state_t st = {0};

    u_get_console_geometry(&cols, &rows, &cw, &ch);

    if (u_mouse_poll(&st) != 0){
        u_printf(4, 4, COL_TEXT, "SYS_MOUSE_POLL: -1 (mouse.drv not loaded or no device)");
        return 1;
    }

    //Хвостовой Enter, которым запускали нас из Сехмет, еще висит в буфере
    //клавиатуры: осушаем буфер, иначе тест закрывается сразу после армирования
    while (u_kbhit()) (void)u_read_scancode();
    uint32_t arm_ticks = get_ticks() + 150;

    u_printf(4, 4, COL_TEXT, "ARHeOS mouse check: move mouse, click buttons. Any key = exit");
    u_printf(4, 400, COL_OK,
             "init: x=%d y=%d b=%x devs=%d",
             st.x, st.y, st.buttons, st.device_count);
    //нарисовать курсор сразу, чтобы видеть, где драйвер его держит
    u_draw_char((uint32_t)st.x, (uint32_t)st.y, '+', COL_TEXT);
    last_px = st.x; last_py = st.y;

    for (;;){
        if (u_mouse_poll(&st) != 0) break;

        if (st.x != last_px || st.y != last_py || st.buttons){
            //перерисовать курсор
            u_clear_tile((uint32_t)last_px, (uint32_t)last_py, COL_BG);
            {
                uint32_t col = COL_TEXT;
                if (st.buttons & U_MOUSE_BTN_LEFT)   col = COL_LEFT;
                if (st.buttons & U_MOUSE_BTN_RIGHT)  col = COL_RIGHT;
                if (st.buttons & U_MOUSE_BTN_MIDDLE) col = COL_MID;
                u_draw_char((uint32_t)st.x, (uint32_t)st.y, '+', col);
            }
            last_px = st.x;
            last_py = st.y;
        }

        //статус печатаем всегда, раз в ~0.5 с: видно и дельту, и покой
        //форматные строки без ширин (%5d и т.п. не поддержаны u_printf и
        //съезжают аргументы) — см. разбор в описании теста
        static uint32_t next_stat = 0;
        if ((int32_t)(get_ticks() - next_stat) >= 0){
            next_stat = get_ticks() + 50;
            u_printf(4, 430, COL_TEXT,
                     "x=%d y=%d dx=%d dy=%d b=%x devs=%d   ",
                     st.x, st.y, st.dx, st.dy, st.buttons, st.device_count);
        }

        if (get_ticks() >= arm_ticks && u_kbhit()) break;
        u_mouse_yield();
    }

    u_clear_tile((uint32_t)last_px, (uint32_t)last_py, COL_BG);
    u_printf(4, 460, COL_TEXT, "mouse check: exit");
    return 0;
}
