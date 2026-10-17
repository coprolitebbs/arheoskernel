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

#include "include/fb_driver.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/font.h"
#include "../../include-kernel/locale_types.h"
//#include "../include_drivers/font_data_driver.h"


uint32_t current_foreground_color = 0x00FFFFFF; //Белый по умолчанию
uint32_t current_background_color = 0x00FF0000; //Красный фон


uint8_t *framebuffer;

uint32_t pitch = 0;
uint32_t bytes_pp = 0;

uint32_t width = 0;
uint32_t height = 0;

uint32_t flags = 0;


uint8_t *fb_wallpaper_raw = NULL;
uint32_t fb_wallpaper_allocated_size = 0;
bool fb_wallpaper_present = false;

fb_driver_api_t api;
fb_driver_api_t *api_ptr;

description_driver_api_t descr_api;


//  ----- внутренние функции  -----

//RGB888 -> RGB565
static uint16_t rgb565(uint32_t color){
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}



//Возвращает codepoint и число съеденных байт
static uint32_t utf8_decode(const char *s, int *out_len){
    const uint8_t *p = (const uint8_t *)s;

    if (p[0] < 0x80){ *out_len = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0){
        *out_len = 2;
        if (!p[1]) return 0xFFFD;
        return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0){
        *out_len = 3;
        if (!p[1] || !p[2]) return 0xFFFD;
        return ((uint32_t)(p[0] & 0x0F) << 12)
             | ((uint32_t)(p[1] & 0x3F) << 6)
             |  (uint32_t)(p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0){
        *out_len = 4;
        if (!p[1] || !p[2] || !p[3]) return 0xFFFD;
        return ((uint32_t)(p[0] & 0x07) << 18)
             | ((uint32_t)(p[1] & 0x3F) << 12)
             | ((uint32_t)(p[2] & 0x3F) << 6)
             |  (uint32_t)(p[3] & 0x3F);
    }
    *out_len = 1;
    return 0xFFFD;   //битый байт
}



// string
void font_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color){
    if (!framebuffer) return;
    while (*str){
        int consumed;
        uint32_t cp = utf8_decode(str, &consumed);
        font_draw_codepoint(x, y, cp, color);
        x += 8;             //моноширинный шаг; потом возьмём из cell_width
        str += consumed;
    }
}





//Внутренний helper - построчное копирование прямоугольника внутри видеопамяти
//Учитывает pitch (который может быть больше w * bytes_pp), обрабатывает
//перекрытие источника и приёмника выбором направления обхода
static void fb_copy_rect_rows(int dst_y, int src_y, int x, int w, int h){
    if (!framebuffer || w <= 0 || h <= 0) return;
    if (dst_y == src_y) return;
    uint32_t row_bytes = (uint32_t)w * bytes_pp;
    uint8_t *base = framebuffer;
    if (dst_y < src_y){
        //Источник ниже приёмника — идём сверху вниз
        for (int i = 0; i < h; i++){
            uint8_t *dst = base + (uint32_t)(dst_y + i) * pitch + (uint32_t)x * bytes_pp;
            uint8_t *src = base + (uint32_t)(src_y + i) * pitch + (uint32_t)x * bytes_pp;
            memmove(dst, src, row_bytes);
        }
    } else {
        // Источник выше приёмника — идём снизу вверх, чтобы не затереть
        for (int i = h - 1; i >= 0; i--){
            uint8_t *dst = base + (uint32_t)(dst_y + i) * pitch + (uint32_t)x * bytes_pp;
            uint8_t *src = base + (uint32_t)(src_y + i) * pitch + (uint32_t)x * bytes_pp;
            memmove(dst, src, row_bytes);
        }
    }
}


//  -----  внешние функции  -----


void fb_put_pixel(int x, int y, uint32_t color){
    if(!framebuffer) return;
    if(x < 0 || y < 0) return;
    if((uint32_t)x >= width) return;
    if((uint32_t)y >= height) return;

    uint8_t *p = framebuffer + y * pitch + x * bytes_pp;
    switch(bytes_pp){
        case 4:
            *(uint32_t*)p = color;
            break;
        case 3:
            p[0] = color & 0xff;
            p[1] = (color >> 8) & 0xff;
            p[2] = (color >> 16) & 0xff;
            break;
        case 2:
            *(uint16_t*)p = rgb565(color);
            break;
        case 1:
            *p = (uint8_t)(color & 0xFF);
            break;
    }
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color){
    if(!framebuffer) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)width)  w = (int)width - x;
    if (y + h > (int)height) h = (int)height - y;
    if (w <= 0 || h <= 0) return;

    uint32_t target_color = color;
    if (bytes_pp == 2){
        target_color = rgb565(color);
    }

    for(int yy = 0; yy < h; yy++){
        uint8_t *line_ptr = framebuffer + ((y + yy) * pitch) + (x * bytes_pp);

        if (bytes_pp == 4){
            uint32_t *ptr = (uint32_t*)line_ptr;
            for(int xx = 0; xx < w; xx++) { ptr[xx] = target_color; }
        }
        else if (bytes_pp == 2){
            uint16_t *ptr = (uint16_t*)line_ptr;
            for(int xx = 0; xx < w; xx++) { ptr[xx] = (uint16_t)target_color; }
        }
        else if (bytes_pp == 3){
            uint8_t r = (target_color >> 16) & 0xFF;
            uint8_t g = (target_color >> 8) & 0xFF;
            uint8_t b = target_color & 0xFF;
            for(int xx = 0; xx < w; xx++){
                line_ptr[0] = b;
                line_ptr[1] = g;
                line_ptr[2] = r;
                line_ptr += 3;
            }
        }
        else if (bytes_pp == 1){
            for(int xx = 0; xx < w; xx++) { line_ptr[xx] = (uint8_t)(target_color & 0xFF); }
        }
    }
}


void fb_clear(uint32_t color){
    //Вычисляем точный размер экрана в байтах на базе pitch
    uint32_t screen_bytes = height * pitch;
    //font_draw_char(400,310,'A',0x00FFFFFF);
    if (fb_wallpaper_present && fb_wallpaper_raw != NULL && framebuffer != NULL){
        //Убеждаемся, что размер выделенной памяти ядра соответствует размеру экрана
        if (fb_wallpaper_allocated_size >= screen_bytes){
            //font_draw_char_in_wp(400,300,'A',0x00FFFFFF);
            memcpy(framebuffer, fb_wallpaper_raw, screen_bytes);
            return;
        }
    }
    fb_fill_rect(0, 0, width, height, color);
}


void fb_upload_wallpaper(const void *src_user_buffer, uint32_t size){
    if (!src_user_buffer || size == 0) return;
    //Предотвращаем аллокацию мусорных размеров (лимит 4 МБ под экраны высокого разрешения)
    if (size > (4 * 1024 * 1024)) return;
    //Если буфер уже существовал — освобождаем старый блок
    if (fb_wallpaper_raw != NULL) {
        //Так как free_ptr указывает на kfree, вызываем только для валидного указателя
        api_ptr->free_ptr(fb_wallpaper_raw);
        fb_wallpaper_raw = NULL; //Защита от Double Free
    }
    //Вызываем ядерный kmalloc
    if (api_ptr->malloc_ptr != NULL){
        fb_wallpaper_raw = (uint8_t *)api_ptr->malloc_ptr(size);
        //font_draw_char(400,310,'B',0x00FFFFFF);
        if (!fb_wallpaper_raw) return; // У ядра кончилась куча Ring 0
        //Переносим пиксели из пространства пользователя напрямую в кучу ядра
        memcpy(fb_wallpaper_raw, src_user_buffer, size);
        fb_wallpaper_allocated_size = size;
        fb_wallpaper_present = true;
    }
}


void fb_set_fg_color(uint32_t color){ current_foreground_color = color; }
void fb_set_bg_color(uint32_t color){ current_background_color = color; }
uint32_t fb_get_fg_color(void){ return current_foreground_color; }
uint32_t fb_get_bg_color(void){ return current_background_color; }

void fb_scroll_up(uint32_t start_y, uint32_t end_y, uint32_t line_height){
    if (!framebuffer) return;
    if (start_y + line_height >= end_y || end_y > height) return;
    //Построчно копируем пиксели видеопамяти снизу вверх
    for (uint32_t y = start_y; y < end_y - line_height; y++){
        uint8_t *dst_line = framebuffer + (y * pitch);
        uint8_t *src_line = framebuffer + ((y + line_height) * pitch);
        memcpy(dst_line, src_line, width * bytes_pp);
    }
    //Освободившуюся нижнюю строчку терминала чисто заливаем цветом текущего фона
    fb_fill_rect(0, end_y - line_height, width, line_height, current_background_color);
}

void fb_clear_tile(int x, int y, uint32_t fallback_color){
    if (!framebuffer) return;
    // Защита от выхода за границы экрана для тайла 8x8
    if (x < 0 || y < 0 || (uint32_t)x + 8 > width || (uint32_t)y + 8 > height) return;
    // Вычисляем размер экрана/строки для копирования
    uint32_t screen_bytes = height * pitch;
    // Если обои загружены и валидны — восстанавливаем пиксели из них
    if (fb_wallpaper_present && fb_wallpaper_raw != NULL && fb_wallpaper_allocated_size >= screen_bytes) {
        for (int yy = 0; yy < 8; yy++){
            //Смещение конкретной строки тайла в общем буфере обоев и во фреймбуфере
            uint32_t offset = (y + yy) * pitch + (x * bytes_pp);
            uint8_t *dst = framebuffer + offset;
            uint8_t *src = fb_wallpaper_raw + offset;
            //Копируем ровно 8 пикселей с учетом глубины цвета
            memcpy(dst, src, 8 * bytes_pp);
        }
    } else {
        //Обоев нет — просто рисуем сплошной квадрат цвета фона консоли
        fb_fill_rect(x, y, 8, 8, fallback_color);
    }
}




void font_draw_codepoint(uint32_t x, uint32_t y, uint32_t cp, uint32_t color){
    if (!framebuffer) return;
    if (!api_ptr || !api_ptr->get_active_locale_ptr || !api_ptr->font_lookup_ptr) return;

    const locale_t *loc = (const locale_t *)api_ptr->get_active_locale_ptr();
    if (!loc || !loc->font) return;

    const font_glyph_t *g =
        (const font_glyph_t *)api_ptr->font_lookup_ptr(loc->font, cp);
    if (!g) return;

    uint8_t bytes_per_row = (g->width + 7) / 8;
    for (uint8_t row = 0; row < g->height; row++){
        const uint8_t *src = g->rows + row * bytes_per_row;
        uint8_t *line_ptr = framebuffer + ((y + row) * pitch) + (x * bytes_pp);

        for (uint8_t col = 0; col < g->width; col++){
            uint8_t byte = src[col >> 3];
            uint8_t bit  = 0x80 >> (col & 7);
            if (byte & bit){
                /* Тот же код записи пикселя, что в старом font_draw_char */
                uint32_t target = color;
                if (bytes_pp == 2) target = rgb565(color);
                if (bytes_pp == 4) *(uint32_t*)line_ptr = target;
                else if (bytes_pp == 3){
                    line_ptr[0] = target & 0xff;
                    line_ptr[1] = (target >> 8) & 0xff;
                    line_ptr[2] = (target >> 16) & 0xff;
                }
                else if (bytes_pp == 2) *(uint16_t*)line_ptr = (uint16_t)target;
                else if (bytes_pp == 1) *line_ptr = (uint8_t)(target & 0xFF);
            }
            line_ptr += bytes_pp;
        }
    }
}


void font_draw_char(uint32_t x, uint32_t y, unsigned char c, uint32_t color){
    font_draw_codepoint(x, y, (uint32_t)c, color);
}


void font_draw_hex_byte(unsigned char byte, int x, int y){
    unsigned char high = (byte >> 4) & 0x0F;
    unsigned char low = byte & 0x0F;
    font_draw_char(x, y, (high < 10) ? ('0' + high) : ('A' + high - 10), 0x00FFFFFF);
    font_draw_char(x + 8, y, (low < 10) ? ('0' + low) : ('A' + low - 10), 0x00FFFFFF);
}

void font_draw_hex_word(uint16_t word, int x, int y){
    font_draw_hex_byte((unsigned char)(word >> 8), x, y);
    font_draw_hex_byte((unsigned char)(word & 0xFF), x + 16, y);
}

void font_draw_hex(uint32_t dword, int x, int y){
    font_draw_hex_word((uint16_t)(dword >> 16), x, y);
    font_draw_hex_word((uint16_t)(dword & 0xFFFF), x + 32, y);
}



void fb_init(struct boot_info *boot){
    framebuffer = (uint8_t*)boot->framebuffer;
    pitch = boot->pitch;
    bytes_pp = boot->bpp / 8;
    width = boot->width;
    height = boot->height;
    flags = boot->graphics_flags;
    //fb_wallpaper_raw = 0;
}


//Сдвиг прямоугольной области фреймбуфера по вертикали
//dy > 0 — содержимое едет вверх, снизу появляется полоса цвета fill
//dy < 0 — содержимое едет вниз, сверху появляется полоса цвета fill
//|dy| >= h — вся область заливается цветом fill
int fb_scroll_rect(int x, int y, int w, int h, int dy, uint32_t fill){
    if (!framebuffer) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if (x + w > (int)width || y + h > (int)height) return -1;
    if (dy == 0) return 0;
    //Полностью вышли за границы прямоугольника — просто затираем
    if (dy >= h || dy <= -h){
        fb_fill_rect(x, y, w, h, fill);
        return 0;
    }
    if (dy > 0){
        //Сдвиг вверх: [y+dy .. y+h) -> [y .. y+h-dy)
        int move_h = h - dy;
        fb_copy_rect_rows(y, y + dy, x, w, move_h);
        //Нижняя полоса [y+move_h .. y+h) — свежая
        fb_fill_rect(x, y + move_h, w, dy, fill);
    } else {
        int ady = -dy;
        //Сдвиг вниз: [y .. y+h-ady) -> [y+ady .. y+h)
        int move_h = h - ady;
        fb_copy_rect_rows(y + ady, y, x, w, move_h);
        //Верхняя полоса [y .. y+ady) — свежая
        fb_fill_rect(x, y, w, ady, fill);
    }
    return 0;
}


//Быстрая копия прямоугольника пикселей из буфера вызывающего в фреймбуфер
//src — упакованные пиксели в формате текущего bpp (bytes_pp байт на пиксель),
//упакованы построчно без промежуточного padding'а (w * bytes_pp байт на строку)
int fb_blit(int x, int y, int w, int h, const void *src){
    if (!framebuffer || !src) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if (x + w > (int)width || y + h > (int)height) return -1;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t row_bytes = (uint32_t)w * bytes_pp;
    for (int i = 0; i < h; i++){
        uint8_t *dst = framebuffer + (uint32_t)(y + i) * pitch + (uint32_t)x * bytes_pp;
        memcpy(dst, s + (uint32_t)i * row_bytes, row_bytes);
    }
    return 0;
}




void driver_main(struct boot_info *boot){

    fb_init(boot);

    description_driver_api_t *descr_api_ptr = &descr_api;
    descr_api_ptr->type = DESCR_FRAMEBUFFER_DRIVER;


    api_ptr = &api;

    api_ptr->put_pixel = fb_put_pixel;
    api_ptr->fill_rect = fb_fill_rect;
    api_ptr->clear = fb_clear;
    api_ptr->draw_char = font_draw_char;
    api_ptr->draw_string = font_draw_string;
    api_ptr->draw_hex = font_draw_hex;
    api_ptr->draw_codepoint = font_draw_codepoint;

    api_ptr->set_fg = fb_set_fg_color;
    api_ptr->set_bg = fb_set_bg_color;
    api_ptr->get_fg = fb_get_fg_color;
    api_ptr->get_bg = fb_get_bg_color;
    api_ptr->scroll_up = fb_scroll_up;

    api_ptr->set_wallpaper = fb_upload_wallpaper;
    api_ptr->clear_tile = fb_clear_tile;

    api_ptr->scroll_rect = fb_scroll_rect;
    api_ptr->blit = fb_blit;

    descr_api_ptr->struct_ptr = (void *)&api;

}


description_driver_api_t *driver_get_api(void){
    return &descr_api;
}



