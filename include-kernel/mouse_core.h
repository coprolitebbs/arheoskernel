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

#ifndef MOUSE_CORE_H
#define MOUSE_CORE_H

#include <stdint.h>
#include "../drivers/include_drivers/drv_format.h"

//Определение в mouse_core.c, присваивается в use_drivers.c при загрузке
//mouse.drv (case DESCR_MOUSE_DRIVER в load_additional_drivers)
extern mouse_driver_api_t *mouse_driver_api;

//Флаг прерывания мыши IRQ12 для isr.c (паттерн floppy_irq_fired)
//Ставится mouse_irq_dispatch() из ISR12, сбрасывается в mouse_core_tick()
extern volatile uint8_t mouse_irq_fired;

//Вызвать из обработчика IRQ12 в isr.c:
//mouse_irq_fired = 1; вызов irq_callback драйвера (вычитка портов)
void mouse_irq_dispatch(void);

//Опрос всех провайдеров и шаг единого курсора (Windows-модель)
//Вызывается из schedule() в task.c по тику таймера, рядом с cleanup_tasks()
//Неблокирующий, только вычитка AUX-буфера 8042 и арифметика
void mouse_core_tick(void);

//Есть ли живой драйвер мыши
int mouse_core_is_ready(void);

int32_t  mouse_core_get_x(void);
int32_t  mouse_core_get_y(void);
uint32_t mouse_core_get_buttons(void);   //MOUSE_BTN_*
int32_t  mouse_core_get_wheel(void);

//Забрать дельту, накопленную с прошлого вызова (для SYS_MOUSE_POLL)
//Возвращает 0 — есть данные, 1 — данных ещё не было (нули), -1 — ошибка
int mouse_core_take_delta(int32_t *dx, int32_t *dy, int32_t *wheel, uint32_t *buttons);

//Установить позицию курсора (SYS_MOUSE_SET_POS), с обрезкой в экран
void mouse_core_set_pos(int32_t x, int32_t y);

#endif
