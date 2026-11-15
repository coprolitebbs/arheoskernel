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

//mouse.h — пользовательские обертки над системными вызовами мыши.
//Структуры дублируют ABI из drivers/include_drivers/drv_format.h
//(Ring-3 не может включать kernel-заголовки напрямую).

#ifndef U_MOUSE_H
#define U_MOUSE_H

#include <stdint.h>

//Номера системных вызовов (syscalls.h ядра). Если в вашей версии ядра
//номера другие — поправьте здесь три константы.
#ifndef SYS_MOUSE_GET
#define SYS_MOUSE_GET     67
#endif
#ifndef SYS_MOUSE_POLL
#define SYS_MOUSE_POLL    68
#endif
#ifndef SYS_MOUSE_SET_POS
#define SYS_MOUSE_SET_POS 69
#endif

//Кнопки (биты в buttons): bit0=L, bit1=R, bit2=M
#define U_MOUSE_BTN_LEFT    0x01
#define U_MOUSE_BTN_RIGHT   0x02
#define U_MOUSE_BTN_MIDDLE  0x04

//Дельта мыши за один опрос (экранные координаты: dy>0 — вниз по экрану)
typedef struct{
    int16_t dx;
    int16_t dy;
    int16_t wheel;
    uint8_t buttons;   //U_MOUSE_BTN_*
} u_mouse_delta_t;

//Состояние единого курсора (модель Windows: все устройства двигают один курсор)
typedef struct{
    int16_t x, y;      //текущая позиция курсора
    int16_t dx, dy;    //дельта, накопленная с предыдущего вызова u_mouse_poll
    uint8_t buttons;
    uint8_t device_count;
} u_mouse_state_t;

//Текущая позиция курсора и кнопки. Возвращает 0 при успехе, -1 — если
//драйвер мыши не загружен. x/y/buttons могут быть 0, если не нужны.
int u_mouse_get(int16_t *x, int16_t *y, uint8_t *buttons);

//Дельта с прошлого вызова + текущее состояние. Возвращает 0/-1.
//ВНИМАНИЕ: u_mouse_get и u_mouse_poll читают один и тот же аккумулятор
//драйвера — используйте в приложении что-то одно.
int u_mouse_poll(u_mouse_state_t *out);

//Телепорт курсора в заданную точку (кламп в границы экрана на месте)
int u_mouse_set_pos(int16_t x, int16_t y);

//Передать квант времени системе — удобно в цикле ожидания движения мыши
void u_mouse_yield(void);

#endif

