#ifndef DRAW_H
#define DRAW_H
#include <stdint.h>

//Глобальные параметры VBE (определены в kernel.c, доступны через extern)
extern unsigned int lfb_addr;
extern unsigned int pitch;
extern unsigned int bytes_pp;

//Графические функции
void draw_char(unsigned int x, unsigned int y, unsigned char c);
void draw_string(unsigned int x, unsigned int y, const char *str);
void fill_screen(unsigned char r, unsigned char g, unsigned char b);

void kernel_draw_char(uint32_t x,uint32_t y,unsigned char c,uint32_t color);
void kernel_fill_screen(uint32_t color);
void kernel_draw_string(uint32_t x,uint32_t y,const char *str,uint32_t color);


#endif
