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

//mouse.c — пользовательские обертки над системными вызовами мыши
//(по стилю аналогично ugraph.c). Линкуется в приложение как u_mouse.o.

#include "../include/mouse.h"
#include "../include/syscall.h"   //SYS_YIELD; если в syscall.h уже есть
                                  //SYS_MOUSE_* — константы возьмутся оттуда

//SYS_MOUSE_GET: eax=номер, -> eax=x (или -1), ebx=y, ecx=buttons
int u_mouse_get(int16_t *x, int16_t *y, uint8_t *buttons)
{
    int rx, ry, rb;
    __asm__ __volatile__("int $0x80"
                         : "=a"(rx), "=b"(ry), "=c"(rb)
                         : "a"(SYS_MOUSE_GET)
                         : "memory");
    if (rx == -1) return -1;          //драйвер мыши не загружен
    if (x)       *x = (int16_t)rx;
    if (y)       *y = (int16_t)ry;
    if (buttons) *buttons = (uint8_t)rb;
    return 0;
}

//SYS_MOUSE_POLL: eax=номер, ebx=u_mouse_state_t* -> eax=0/-1
int u_mouse_poll(u_mouse_state_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(SYS_MOUSE_POLL), "b"(out)
                         : "memory");
    return ret;
}

//SYS_MOUSE_SET_POS: eax=номер, ebx=x, ecx=y -> eax=0/-1
int u_mouse_set_pos(int16_t x, int16_t y)
{
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(SYS_MOUSE_SET_POS), "b"((int)x), "c"((int)y)
                         : "memory");
    return ret;
}

//SYS_YIELD — обертка для циклов ожидания событий мыши
void u_mouse_yield(void)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_YIELD) : "memory");
}


