#include "include-kernel/debug.h"
#include <stdint.h>
#include "include-kernel/draw.h"
#include "include-kernel/isr.h"


unsigned int dbgp = 0;

void draw_hex_dump(uint32_t addr, int num_bytes, int x, int y){
    unsigned char *data = (unsigned char*)addr;
    for (int i = 0; i < num_bytes; i++) {
        unsigned char c = data[i];
        unsigned char high = (c >> 4) & 0x0F;
        unsigned char low = c & 0x0F;
        draw_char(x, y, (high < 10) ? ('0' + high) : ('A' + high - 10));
        draw_char(x + 8, y, (low < 10) ? ('0' + low) : ('A' + low - 10));
        x += 16; //пробел между байтами
        if ((i + 1) % 8 == 0) {
            x = 0;
            y += 8;
        }
    }
}


void draw_hex_byte(unsigned char byte, int x, int y){
    unsigned char high = (byte >> 4) & 0x0F;
    unsigned char low = byte & 0x0F;
    draw_char(x, y, (high < 10) ? ('0' + high) : ('A' + high - 10));
    draw_char(x + 8, y, (low < 10) ? ('0' + low) : ('A' + low - 10));
}

void draw_hex_word(uint16_t word, int x, int y){
    draw_hex_byte((unsigned char)(word >> 8), x, y);
    draw_hex_byte((unsigned char)(word & 0xFF), x + 16, y);
}

void draw_hex_dword(uint32_t dword, int x, int y){
    //kernel_schedule_lock = 1;
    draw_hex_word((uint16_t)(dword >> 16), x, y);
    draw_hex_word((uint16_t)(dword & 0xFFFF), x + 32, y);
    //kernel_schedule_lock = 0;
}


void _test(void){
	return;
}


void draw_hex_dump_spaced(const void *addr, int num_bytes, int x, int y){
    const unsigned char *data = (const unsigned char*)addr;
    int stx = x;

    for(int i=0;i<num_bytes;i++){
        unsigned char c=data[i];
        draw_hex_byte(c,x,y);
        x+=24;
        if((i+1)%8==0){
            x = stx;
            y += 10;
        }
    }
}

void draw_char_spaced(const void *addr, int num_bytes, int x, int y){
    const unsigned char *data = (const unsigned char*)addr;
    int stx = x;
    for(int i=0;i<num_bytes;i++){
        unsigned char c=data[i];
        draw_char(x,y,c);
        x += 10;
        if((i+1)%8==0){
            x = stx;
            y += 10;
        }
    }
}
