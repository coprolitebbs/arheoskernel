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

#ifndef GRAPHIC_LAYER_H
#define GRAPHIC_LAYER_H

#include "../drivers/include_drivers/driver_graphic_layer.h"

//================= API слоя (реализовано в graphic_layer.c) ============
//Вызывается только из кода ядра. Драйверы получают этот слой неявно:
//их gr_driver_t регистрируется ядром после загрузки ELF-модуля

//Инициализация слоя. Вызывается ядром из drivers_init().
//Если внешние графические драйверы уже зарегистрированы — активируется
//старший по priority; иначе подключается fb-совместимый fallback поверх
//штатного fb_driver (все недостающие функции — заглушки/софт-реализации)
void init_graphic_layer(void);

//реестр драйверов
int graphic_layer_register_driver(gr_driver_t *drv);    //автопереключение по priority
int graphic_layer_unregister_driver(const char *name);
int graphic_layer_activate_driver(const char *name);    //ручное переключение
gr_driver_t *graphic_layer_get_active(void);
uint32_t graphic_layer_get_driver_count(void);

//перечисление/смена режимов активного драйвера
int gr_get_mode_count(void);
int gr_enum_mode(uint32_t index, gr_mode_info_t *out);
int gr_set_mode(uint32_t index);
int gr_get_current_mode(gr_mode_info_t *out);
int gr_get_fix_info(gr_fix_info_t *out);

//примитивы: работают всегда, недостающие операции драйвера заменяются
//программными реализациями слоя (через LFB текущего режима)
int gr_put_pixel(int x, int y, uint32_t color);
int gr_get_pixel(int x, int y, uint32_t *out);
int gr_fill_rect(int x, int y, int w, int h, uint32_t color);
int gr_clear(uint32_t color);
int gr_hline(int x, int y, int w, uint32_t color);
int gr_vline(int x, int y, int h, uint32_t color);
int gr_draw_rect(int x, int y, int w, int h, uint32_t color);       //рамка
int gr_draw_line(int x0, int y0, int x1, int y1, uint32_t color);   //Брезенхэм
int gr_blit(int x, int y, int w, int h, const void *src);           //RAM -> FB
int gr_copy_area(int sx, int sy, int w, int h, int dx, int dy);     //внутри FB

//управление выводом
int gr_pan_display(uint32_t yoffset);
int gr_blank(int blank);
int gr_wait_vsync(void);
int gr_set_palette(uint32_t start, uint32_t count, const uint8_t *rgb);

#endif
