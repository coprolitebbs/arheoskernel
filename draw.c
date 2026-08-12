#include <stdint.h>
#include "include-kernel/draw.h"
#include "include-kernel/font_data.h"   // массив font_data[256][8]
#include "include-kernel/bootinfo.h"
#include "include-kernel/use_drivers.h"

unsigned int lfb_addr;
unsigned int pitch;
unsigned int bytes_pp;

// ---- Рисование символа ----
void draw_char(unsigned int x, unsigned int y, unsigned char c) {
    if (pitch == 0 || lfb_addr == 0) return;
	unsigned char *fb = (unsigned char*)lfb_addr;
    unsigned char *glyph = font_data[c];
    unsigned int row_offset = y * pitch;

    for (int row = 0; row < 8; row++) {
        unsigned int line_offset = row_offset + row * pitch + x * bytes_pp;
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & 0x80) {
                fb[line_offset + col * bytes_pp] = 0xFF;     // B
                fb[line_offset + col * bytes_pp + 1] = 0xFF; // G
                fb[line_offset + col * bytes_pp + 2] = 0xFF; // R
                // Для 32-bit четвёртый байт не трогаем
            }
            bits <<= 1;
        }
    }
}

// ---- Рисование строки ----
void draw_string(unsigned int x, unsigned int y, const char *str) {
    while (*str) {
        draw_char(x, y, *str);
        x += 8;
        str++;
    }
}

// ---- Заливка экрана цветом (RGB) ----
void fill_screen(unsigned char r, unsigned char g, unsigned char b) {
	unsigned char *fb = (unsigned char*)lfb_addr;
	//unsigned char *fb = (unsigned char*)boot_info->framebuffer;
	unsigned int bytes_pp  = boot_info->bpp / 8;
    for (int y = 0; y < 768; y++) {
        for (int x = 0; x < 1024; x++) {
            int offset = y * pitch + x * bytes_pp;
            fb[offset] = b;
            fb[offset + 1] = g;
            fb[offset + 2] = r;
            // Для 32-bit четвёртый байт не трогаем
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

void kernel_draw_string(uint32_t x,uint32_t y,const char *str,uint32_t color){
    if(fb && fb->draw_string){
        fb->draw_string(x, y, str, color);
    }
}

