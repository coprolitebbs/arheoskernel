#include "include/fb_driver.h"
#include "../../include-kernel/bootinfo.h"
#include "../include_drivers/font_data_driver.h"


uint8_t *framebuffer;

uint32_t pitch = 0;
uint32_t bytes_pp = 0;

uint32_t width = 0;
uint32_t height = 0;

fb_driver_api_t api;


//  ----- внутренние функции  -----

// --------------------------------------------------
// RGB888 -> RGB565
// --------------------------------------------------

static uint16_t rgb565(uint32_t color){
    uint8_t r = (color >> 16) & 0xFF;

    uint8_t g = (color >> 8) & 0xFF;

    uint8_t b = color & 0xFF;


    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// --------------------------------------------------
// pixel
// --------------------------------------------------

static void put_pixel(uint32_t x,uint32_t y,uint32_t color){

    if(x >= width || y >= height) return;
    uint8_t *p = framebuffer + y * pitch + x * bytes_pp;
    if(bytes_pp == 4){
        *(uint32_t*)p = color;
    }
    else if(bytes_pp == 3){
        p[0]=color & 0xff;
        p[1]=(color>>8)&0xff;
        p[2]=(color>>16)&0xff;
    }
    else if(bytes_pp == 2){
        *(uint16_t*)p = rgb565(color);
    }
}

// --------------------------------------------------
// char 8x8
// --------------------------------------------------

void font_draw_char(uint32_t x,uint32_t y,unsigned char c,uint32_t color){
    const unsigned char *glyph = font_data_driver[c];
    for(uint32_t row=0; row<8; row++){
        unsigned char bits = glyph[row];
        for(uint32_t col=0; col<8; col++){
            if(bits & 0x80){
                put_pixel(x+col,y+row,color);
            }
            bits <<= 1;
        }
    }
}

// --------------------------------------------------
// string
// --------------------------------------------------

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

void font_draw_hex_word(uint16_t word, int x, int y) {
    font_draw_hex_byte((unsigned char)(word >> 8), x, y);
    font_draw_hex_byte((unsigned char)(word & 0xFF), x + 16, y);
}

void font_draw_hex(uint32_t dword, int x, int y) {
    font_draw_hex_word((uint16_t)(dword >> 16), x, y);
    font_draw_hex_word((uint16_t)(dword & 0xFFFF), x + 32, y);
}


//  -----  внешние функции  -----

void fb_init(struct boot_info *boot){
    framebuffer = (uint8_t*)boot->framebuffer;
    pitch = boot->pitch;
    bytes_pp = boot->bpp / 8;
    width = boot->width;
    height = boot->height;
}

void fb_put_pixel(int x,int y,uint32_t color){
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
            *(uint16_t*)p = (uint16_t)color;
            break;
    }
}

void fb_fill_rect(int x,int y,int w,int h,uint32_t color){
    for(int yy=0; yy<h; yy++){
        for(int xx=0; xx<w; xx++){
            fb_put_pixel(x + xx,y + yy,color);
        }
    }
}

void fb_clear(uint32_t color){
    fb_fill_rect(0,0,width,height,color);
}

void driver_main(struct boot_info *boot){
    fb_init(boot);

    fb_driver_api_t *a = &api;

    a->put_pixel = fb_put_pixel;
    a->fill_rect = fb_fill_rect;
    a->clear = fb_clear;
    a->draw_char = font_draw_char;
    a->draw_string = font_draw_string;
    a->draw_hex = font_draw_hex;
}

fb_driver_api_t *driver_get_api(void){
    return &api;
}
