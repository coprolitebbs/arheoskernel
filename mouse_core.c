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

//mouse_core.c — ядерная надстройка над mouse-драйвером
//Хранит абсолютные координаты единого курсора (Windows-модель: дельты от всех зарегистрированных
//провайдеров суммируются), кнопки и колесо.
//Точки вызова:
//   - mouse_core_tick()      из schedule() в task.c (тик таймера, cli).
//   - mouse_irq_dispatch()   из ISR12 (если реализован опрос из isr_handler в isr.c)
//   - mouse_core_*()         из systemcall_mouse_* в syscalls.c
//Всё неблокирующее: драйверный poll() только вычитывает буфер 8042

#include "include-kernel/mouse_core.h"
#include "include-kernel/bootinfo.h"

mouse_driver_api_t *mouse_driver_api = 0;

volatile uint8_t mouse_irq_fired = 0;

//Абсолютное состояние курсора. Начальная позиция уточняется клампом по реальному разрешению в первом же tick()
static int32_t mc_x = 160;
static int32_t mc_y = 100;
static int32_t mc_wheel = 0;
static uint32_t mc_buttons = 0;

//Дельта, накопленная с прошлого SYS_MOUSE_POLL
static int32_t acc_dx = 0;
static int32_t acc_dy = 0;
static int32_t acc_wheel = 0;

static uint8_t mc_ever_polled = 0;

static void mc_clamp(void){
    if (!boot_info) return;
    int32_t w = (int32_t)boot_info->width;
    int32_t h = (int32_t)boot_info->height;
    if (w < 1 || h < 1) return;
    if (mc_x < 0)      mc_x = 0;
    if (mc_x > w - 1)  mc_x = w - 1;
    if (mc_y < 0)      mc_y = 0;
    if (mc_y > h - 1)  mc_y = h - 1;
}

void mouse_core_tick(void){
    if (!mouse_driver_api || !mouse_driver_api->enabled) return;

    mouse_delta_t d;
    if (mouse_driver_api->poll(&d) != FS_OK) return;

    acc_dx    += d.dx;
    acc_dy    += d.dy;
    acc_wheel += d.wheel;

    mc_x      += d.dx;
    mc_y      += d.dy;
    mc_wheel  += d.wheel;
    mc_buttons = d.buttons;

    mc_clamp();
    mc_ever_polled = 1;
    mouse_irq_fired = 0;
}

void mouse_irq_dispatch(void){
    mouse_irq_fired = 1;
    if (mouse_driver_api && mouse_driver_api->irq_callback)
        mouse_driver_api->irq_callback();
}

int mouse_core_is_ready(void){
    return (mouse_driver_api && mouse_driver_api->enabled) ? 1 : 0;
}

int32_t mouse_core_get_x(void){
    return mc_x;
}

int32_t mouse_core_get_y(void){
    return mc_y;
}

uint32_t mouse_core_get_buttons(void){
    return mc_buttons;
}

int32_t mouse_core_get_wheel(void){
    return mc_wheel;
}

int mouse_core_take_delta(int32_t *dx, int32_t *dy, int32_t *wheel, uint32_t *buttons){
    if (!dx || !dy || !wheel || !buttons) return -1;
    *dx = acc_dx; *dy = acc_dy; *wheel = acc_wheel; *buttons = mc_buttons;
    acc_dx = 0; acc_dy = 0; acc_wheel = 0;
    return mc_ever_polled ? FS_OK : 1;
}

void mouse_core_set_pos(int32_t x, int32_t y){
    mc_x = x; mc_y = y;
    mc_clamp();
}
