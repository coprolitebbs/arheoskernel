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

#include <stdint.h>
#include "include-kernel/draw.h"
#include "include-kernel/font_data.h"   //массив font_data[256][8]
#include "include-kernel/bootinfo.h"
#include "include-kernel/use_drivers.h"
#include "include-kernel/isr.h"
#include "include-kernel/pmm.h"
#include "include-kernel/vmm.h"
#include "include-kernel/comdebug.h"


unsigned int lfb_addr;
unsigned int pitch;
unsigned int bytes_pp;
uint32_t back_buffer[1024 * 768];


void back_buffer_draw_pixel(int x, int y, uint32_t color){
    if (x >= 0 && x < 1024 && y >= 0 && y < 768){
        back_buffer[y * 1024 + x] = color;
    }
}


void back_buffer_screen_flip(void){
    volatile uint32_t* dest = (volatile uint32_t*)fb_phys_addr;//0xDF000000;
    uint32_t* src = (uint32_t*)back_buffer;
    //Нужно скопировать (1024 * 768 * 2) байт
    //В одном uint32_t содержится 4 байта
    //Следовательно, итераций цикла должно быть: (1024 * 768 * 2) / 4 = 393216
    for (uint32_t i = 0; i < 393216; i++){
        dest[i] = src[i];
    }
}

//Рисование символа
//c — байт в CP866. Для ASCII (0x00..0x7F) совпадает с codepoint
//Для codepoint-based рендера использовать fb->draw_codepoint
void _draw_char(unsigned int x, unsigned int y,
                unsigned char c,
                unsigned char r, unsigned char g, unsigned char b,
                int bg_transparent,
                unsigned char bg_r, unsigned char bg_g, unsigned char bg_b)
{
    if (!boot_info) return;

    //Извлекаем параметры из структуры boot_info
    uint32_t width = boot_info->width;
    uint32_t height = boot_info->height;
    uint32_t pitch = boot_info->pitch;
    uint32_t bpp = boot_info->bpp;
    uint32_t flags = boot_info->graphics_flags;
    uint32_t lfb = boot_info->framebuffer;

    //Граничные проверки для предотвращения краша ядра
    if (x + 8 > width || y + 8 > height) return;
    if (bpp == 0 || bpp > 32) bpp = 16;
    uint32_t bytes_pp = (bpp + 7) / 8;

    //Определяем базовый адрес (LFB или UMA)
    uint8_t *fb_ptr = NULL;
    int has_lfb = (flags & 0x1);

    if (has_lfb) {
        if (lfb == 0) return;
        fb_ptr = (uint8_t *)lfb;
    } else {
        uint32_t addr = graphics_get_fb_virt_addr();
        if (addr == 0) return;
        fb_ptr = (uint8_t *)addr;
    }

    //Подготавливаем значения цветов текста и фона для разных BPP
    uint32_t fg_color32 = 0;
    uint16_t fg_color16 = 0;
    uint8_t  fg_color8  = 0xFF; // Белый для 8-бит по умолчанию

    uint32_t bg_color32 = 0;
    uint16_t bg_color16 = 0;
    uint8_t  bg_color8  = 0x00; // Черный для 8-бит по умолчанию

    if (bpp == 16){
        fg_color16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        bg_color16 = ((bg_r >> 3) << 11) | ((bg_g >> 2) << 5) | (bg_b >> 3);
    } else if (bpp == 24 || bpp == 32) {
        fg_color32 = b | (g << 8) | (r << 16);
        bg_color32 = bg_b | (bg_g << 8) | (bg_r << 16);
    }

    //Получаем битовый паттерн символа из шрифта
    unsigned char *glyph = font_data[c];

    //Построчный цикл отрисовки глифа (8 строк по 8 пикселей)
    for (int row = 0; row < 8; row++) {
        //Вычисляем указатель на начало конкретной строки на экране
        uint8_t *line_ptr = fb_ptr + ((y + row) * pitch) + (x * bytes_pp);
        unsigned char bits = glyph[row];

        for (int col = 0; col < 8; col++) {
            int is_fg_pixel = (bits & 0x80);
            //Если пиксель фоновый и включена прозрачность — просто пропускаем шаг
            if (!is_fg_pixel && bg_transparent) {
                line_ptr += bytes_pp;
                bits <<= 1;
                continue;
            }
            //Отрисовка в зависимости от битности режима
            if (bpp == 16) {
                *(uint16_t *)line_ptr = is_fg_pixel ? fg_color16 : bg_color16;
            }
            else if (bpp == 32) {
                *(uint32_t *)line_ptr = is_fg_pixel ? fg_color32 : bg_color32;
            }
            else if (bpp == 24) {
                //В 24-битном режиме адресация идет строго побайтово
                if (is_fg_pixel) {
                    line_ptr[0] = b;
                    line_ptr[1] = g;
                    line_ptr[2] = r;
                } else {
                    line_ptr[0] = bg_b;
                    line_ptr[1] = bg_g;
                    line_ptr[2] = bg_r;
                }
            }
            else {// 8 bit Indexed
                *line_ptr = is_fg_pixel ? fg_color8 : bg_color8;
            }

            line_ptr += bytes_pp; //Сдвигаем указатель на следующий пиксель в строке
            bits <<= 1;           //Сдвигаем маску глифа влево
        }
    }
}


void draw_char(unsigned int x, unsigned int y, unsigned char c){
    _draw_char(x,y,c,0xFF,0xFF,0xFF,1,0,0,0);
}


//Рисование строки
void draw_string(unsigned int x, unsigned int y, const char *str){
    while (*str){
        draw_char(x, y, *str);
        x += 8;
        str++;
    }
}


//Заливка экрана цветом (RGB)
void fill_screen(unsigned char r, unsigned char g, unsigned char b) {
    if (!boot_info) return;
    uint32_t lfb = boot_info->framebuffer;
    uint32_t width = boot_info->width;
    uint32_t height = boot_info->height;
    uint32_t pitch = boot_info->pitch;
    uint32_t bpp = boot_info->bpp;
    uint32_t flags = boot_info->graphics_flags;

    // Проверка флага LFB (предположим, бит 0)
    int has_lfb = (flags & 0x1);

    uint8_t *fb_ptr = NULL;

    uint32_t addr = 0;

    if (has_lfb) {
        //Режим 1 - LFB активен, используем прямой адрес
        if (lfb == 0) return; //Ошибка инициализации
        fb_ptr = (uint8_t *)lfb;
    } else {
        //Режим 2 - LFB нет, используем UMA fallback

        addr = graphics_get_fb_virt_addr();
        if (addr == 0) {
            com_puts("[FILL_SCREEN] ERROR: Framebuffer address is 0!\n");
            return;
        }

        fb_ptr = (uint8_t *)addr;

        com_puts("[FILL_SCREEN] Starting fill at 0x");
        com_puthex(addr);
        com_puts("\n");

        //В режиме без LFB на UMA память часто линейна относительно начала видео-буфера,
        //даже если BIOS называет это "banked".
        //При переключении в графический режим VESA без флага LFB,
        //доступ к памяти по вычисленному адресу (если он физически верный)
        //должен работать напрямую в защищенном режиме.
        //Pitch в этом случае может быть просто шириной * bpp, но, все же, лучше доверять boot->pitch.
    }

    if (bpp == 0 || bpp > 32) bpp = 16; // Защита
    com_puts("[FILL] Addr: "); com_puthex(addr);
    com_puts(" W: "); com_puthex(width);
    com_puts(" H: "); com_puthex(height);
    com_puts(" Pitch: "); com_puthex(pitch);
    com_puts(" BPP: "); com_puthex(bpp);
    com_puts(" Bytes PP: "); com_puthex(bytes_pp);
    com_puts("\n");

    //Подготовка цветов
    uint16_t color_16 = 0;
    uint32_t color_32 = 0;
    uint8_t color_8 = 0xFF;

    if (bpp == 16){
        //RGB565
        color_16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    } else if (bpp == 24 || bpp == 32){
        //BGR Little Endian
        color_32 = b | (g << 8) | (r << 16);
    }

    //Цикл отрисовки
    for (uint32_t y = 0; y < height; y++){
        uint8_t *line = fb_ptr + (y * pitch);

        if (bpp == 16){
            uint16_t *ptr = (uint16_t *)line;
            for (uint32_t x = 0; x < width; x++){
                ptr[x] = color_16;
            }
        } else if (bpp == 32){
            uint32_t *ptr = (uint32_t *)line;
            for (uint32_t x = 0; x < width; x++){
                ptr[x] = color_32;
            }
        } else if (bpp == 24){
            uint8_t *ptr = line;
            for (uint32_t x = 0; x < width; x++){
                ptr[0] = b;
                ptr[1] = g;
                ptr[2] = r;
                ptr += 3;
            }
        } else {
            // 8 bit
            for (uint32_t x = 0; x < width; x++){
                line[x] = color_8;
            }
        }
    }
}


void kernel_draw_char(uint32_t x,uint32_t y,unsigned char c,uint32_t color){
    if(fb && fb->draw_char){
        fb->draw_char(x,y,c,color);
    }
}


void kernel_fill_screen(uint32_t color){
    if(fb && fb->clear){
        fb->clear(color);
    }
}


//Fallback: fb-драйвер ещё не загружен.
//Работаем через font_data[] — таблицу CP866-шрифта из BIOS.
//Всё, что приходит, конвертируется в CP866-байты для _draw_char.
//Это низкоуровневый путь для ранних паник; после init_drivers() сюда
//попасть уже нельзя — fb->draw_string уйдёт в font_draw_codepoint.
void kernel_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color){
    if (fb && fb->draw_string){
        fb->draw_string(x, y, str, color);
    } else {
        //Драйвер фреймбуфера еще не загружен. Используем низкоуровневую функцию _draw_char(),
        //которая пишет напрямую по физическому адресу LFB из boot_info.
        //Добавляем локальный автомат UTF-8 -> CP866, чтобы ранняя отладка кириллицы работала
        uint8_t lead_byte = 0;
        while (*str){
            uint8_t b = (uint8_t)*str;
            //Ловим стартовый байт русской буквы в UTF-8
            if (b == 0xD0 || b == 0xD1){
                lead_byte = b;
                str++;
                continue;
            }
            //Если есть сохраненный стартовый байт — транслируем кириллицу в CP866
            if (lead_byte != 0){
                uint8_t cp866_char = ' ';
                if (lead_byte == 0xD0){
                    if (b >= 0x90 && b <= 0xBF){
                        if (b <= 0xAF) cp866_char = b - 0x90 + 0x80;
                        else           cp866_char = b - 0xB0 + 0xA0;
                    }
                    else if (b == 0x81) cp866_char = 0xF0; //'Ё'
                }
                else if (lead_byte == 0xD1){
                    if (b >= 0x80 && b <= 0x8F)  cp866_char = b - 0x80 + 0xE0;
                    else if (b == 0x91)          cp866_char = 0xF1; //'ё'
                }
                b = cp866_char;
                lead_byte = 0;
            }
            //Вызываем _draw_char с дефолтным белым цветом (0xFF, 0xFF, 0xFF),
            //прозрачным фоном (1) и черной подложкой
            _draw_char(x, y, b, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0);
            x += 8; //Сдвиг сетки строго на 1 символ шрифта
            str++;
        }
    }
}
