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

//mouse_check.c — тест драйвера мыши ARHeOS.
//Рисует курсор '+' по данным SYS_MOUSE_GET, показывает координаты и кнопки.
//Выход — любая клавиша. Сборка: u_std.o + u_mouse.o + u_mouse_check.o.

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
    int16_t x = 0, y = 0;
    uint8_t b = 0;
    int last_px = -1, last_py = -1;

    u_get_console_geometry(&cols, &rows, &cw, &ch);

    if (u_mouse_get(&x, &y, &b) != 0){
        u_printf(4, 4, COL_TEXT, "SYS_MOUSE_GET: -1 (mouse.drv not loaded or no device)");
        return 1;
    }

    u_printf(4, 4, COL_TEXT, "ARHeOS mouse check: move mouse, click buttons. Any key = exit");
    u_printf(4, 4 + (int)ch, COL_OK, "cursor init: x=%d y=%d buttons=%02x", x, y, b);

    for (;;){
        int16_t nx, ny;
        uint8_t nb;

        u_mouse_get(&nx, &ny, &nb);

        if (nx != x || ny != y || nb != b){
            //перерисовать курсор
            if (last_px >= 0) u_clear_tile((uint32_t)last_px, (uint32_t)last_py, COL_BG);
            {
                uint32_t col = COL_TEXT;
                if (nb & U_MOUSE_BTN_LEFT)   col = COL_LEFT;
                if (nb & U_MOUSE_BTN_RIGHT)  col = COL_RIGHT;
                if (nb & U_MOUSE_BTN_MIDDLE) col = COL_MID;
                u_draw_char((uint32_t)nx, (uint32_t)ny, '+', col);
            }
            last_px = nx;
            last_py = ny;
            x = nx; y = ny; b = nb;

            u_printf(4, 4 + 2 * (int)ch, COL_TEXT,
                     "x=%5d y=%5d buttons=%02x %s%s%s   ",
                     x, y, b,
                     (b & U_MOUSE_BTN_LEFT)  ? "L" : "-",
                     (b & U_MOUSE_BTN_RIGHT) ? "R" : "-",
                     (b & U_MOUSE_BTN_MIDDLE)? "M" : "-");
        }

        if (u_kbhit()) break;
        u_mouse_yield();
    }

    if (last_px >= 0) u_clear_tile((uint32_t)last_px, (uint32_t)last_py, COL_BG);
    u_printf(4, 4 + 3 * (int)ch, COL_TEXT, "mouse check: exit");
    return 0;
}
