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

/*
 * mtest.c — тест драйвера мыши.
 *
 * Опрашивает SYS_MOUSE_GET в цикле и рисует метку курсора в framebuffer
 * через SYS_FB_BLIT: белая метка на текущей позиции, серый след на прежней.
 * При зажатой левой кнопке метка становится красной.
 *
 * Если mouse.drv не загружен (SYS_MOUSE_GET возвращает -1), приложение
 * просто ждёт и продолжает опрос — можно запустить до загрузки драйвера
 * и перезапустить после.
 *
 * Сборка : ELF PIE, линкуется с u_std_elf.o.
 */

#include <stdint.h>
#include "../../include/syscall.h"

//---- Прямые системные вызовы ----

static inline int mt_mouse_get(int32_t *x, int32_t *y, uint32_t *btn){
    uint32_t a, b, c, d;
    asm volatile("int $0x80"
                 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                 : "a"(SYS_MOUSE_GET)
                 : "memory");
    if ((int32_t)a == -1) return -1;
    if (x)    *x = (int32_t)b;
    if (y)    *y = (int32_t)c;
    if (btn)  *btn = d;
    return 0;
}

static inline int mt_blit(int32_t x, int32_t y, int32_t w, int32_t h, const void *src){
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(SYS_FB_BLIT), "b"(x), "c"(y), "d"(w), "S"(h), "D"(src)
                 : "memory");
    return ret;
}

static inline void mt_yield(void){
    asm volatile("int $0x80" :: "a"(SYS_YIELD) : "memory");
}

//---- Отрисовка метки курсора ----

#define CURSOR_BOX 4    //размер метки, пикселей

static uint32_t box_colors[CURSOR_BOX * CURSOR_BOX];
static uint32_t trail_colors[2 * 2];

static void fill_colors(uint32_t color){
    for (int i = 0; i < CURSOR_BOX * CURSOR_BOX; i++) box_colors[i] = color;
}

static void draw_box(int32_t x, int32_t y, uint32_t color){
    fill_colors(color);
    mt_blit(x, y, CURSOR_BOX, CURSOR_BOX, box_colors);
}

static void draw_trail(int32_t x, int32_t y, uint32_t color){
    trail_colors[0] = color; trail_colors[1] = color;
    trail_colors[2] = color; trail_colors[3] = color;
    mt_blit(x, y, 2, 2, trail_colors);
}

#define COLOR_WHITE   0x00FFFFFF
#define COLOR_GREY    0x00606060
#define COLOR_RED     0x00FF4040

int main(void){
    int32_t px = -1, py = -1;      //прошлые координаты
    uint32_t pbtn = 0xFFFFFFFF;    //прошлые кнопки

    for (;;){
        int32_t x, y;
        uint32_t btn;
        int r = mt_mouse_get(&x, &y, &btn);

        if (r == 0 && (x != px || y != py || btn != pbtn)){
            //На прежнем месте оставляем серый след (видна траектория)
            if (px >= 0 && py >= 0) draw_trail(px, py, COLOR_GREY);
            //Новая позиция: красная при зажатой левой кнопке, иначе белая
            draw_box(x, y, (btn & 0x01) ? COLOR_RED : COLOR_WHITE);
            px = x; py = y; pbtn = btn;
        }

        mt_yield();
    }

    return 0;
}
