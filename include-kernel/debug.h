#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include <stddef.h>   // для size_t

extern unsigned int dbgp;

void draw_hex_dump(uint32_t addr, int num_bytes, int x, int y);
void draw_hex_byte(unsigned char byte, int x, int y);
void draw_hex_word(uint16_t word, int x, int y);
void draw_hex_dword(uint32_t dword, int x, int y);

void _test(void); //Точка стопа для LLDB

void draw_hex_dump_spaced(const void *addr, int num_bytes, int x, int y);
void draw_char_spaced(const void *addr, int num_bytes, int x, int y);

#endif
