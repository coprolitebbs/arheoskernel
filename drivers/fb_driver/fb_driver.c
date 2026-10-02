#include "include/fb_driver.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"
#include "../include_drivers/font_data_driver.h"


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


//  ----- внутренние функции  -----

// RGB888 -> RGB565

static uint16_t rgb565(uint32_t color){
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// pixel
/*static void put_pixel(uint32_t x, uint32_t y, uint32_t color){
    if(x >= width || y >= height) return;
    uint8_t *p = framebuffer + y * pitch + x * bytes_pp;
    if(bytes_pp == 4){
        *(uint32_t*)p = color;
    }
    else if(bytes_pp == 3){
        p[0] = color & 0xff;
        p[1] = (color >> 8) & 0xff;
        p[2] = (color >> 16) & 0xff;
    }
    else if(bytes_pp == 2){
        *(uint16_t*)p = rgb565(color);
    }
    else if(bytes_pp == 1){
        *p = (uint8_t)(color & 0xFF); // Поддержка 8-бит
    }
}*/

//char 8x8
void font_draw_char(uint32_t x, uint32_t y, unsigned char c, uint32_t color){
    if (!framebuffer) return;
    if (x + 8 > width || y + 8 > height) return;
    //Сжимаем цвет один раз для всего глифа
    uint32_t target_color = color;
    if (bytes_pp == 2){
        target_color = rgb565(color);
    }
    const unsigned char *glyph = font_data_driver[c];
    for(uint32_t row = 0; row < 8; row++){
        //Вычисляем точный базовый адрес начала строки символа на экране
        uint8_t *line_ptr = framebuffer + ((y + row) * pitch) + (x * bytes_pp);
        unsigned char bits = glyph[row];

        for(uint32_t col = 0; col < 8; col++){
            if(bits & 0x80){
                if(bytes_pp == 4){
                    *(uint32_t*)line_ptr = target_color;
                }
                else if(bytes_pp == 3){
                    line_ptr[0] = target_color & 0xff;
                    line_ptr[1] = (target_color >> 8) & 0xff;
                    line_ptr[2] = (target_color >> 16) & 0xff;
                }
                else if(bytes_pp == 2){
                    *(uint16_t*)line_ptr = (uint16_t)target_color;
                }
                else if(bytes_pp == 1){
                    *line_ptr = (uint8_t)(target_color & 0xFF);
                }
            }
            line_ptr += bytes_pp; // Линейный сдвиг указателя вправо
            bits <<= 1;
        }
    }
}

// string
void font_draw_string(uint32_t x,uint32_t y,const char *str,uint32_t color){
    while(*str){
        font_draw_char(x, y,(unsigned char)*str,color);
        x += 8;
        str++;
    }
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



void fb_init(struct boot_info *boot){
    framebuffer = (uint8_t*)boot->framebuffer;
    pitch = boot->pitch;
    bytes_pp = boot->bpp / 8;
    width = boot->width;
    height = boot->height;
    flags = boot->graphics_flags;
    //fb_wallpaper_raw = 0;
}


void driver_main(struct boot_info *boot){

    fb_init(boot);

    api_ptr = &api;

    api_ptr->put_pixel = fb_put_pixel;
    api_ptr->fill_rect = fb_fill_rect;
    api_ptr->clear = fb_clear;
    api_ptr->draw_char = font_draw_char;
    api_ptr->draw_string = font_draw_string;
    api_ptr->draw_hex = font_draw_hex;

    api_ptr->set_fg = fb_set_fg_color;
    api_ptr->set_bg = fb_set_bg_color;
    api_ptr->get_fg = fb_get_fg_color;
    api_ptr->get_bg = fb_get_bg_color;
    api_ptr->scroll_up = fb_scroll_up;

    api_ptr->set_wallpaper = fb_upload_wallpaper;
    api_ptr->clear_tile = fb_clear_tile;
}

fb_driver_api_t *driver_get_api(void){
    return &api;
}



