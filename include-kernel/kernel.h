#ifndef KERNEL_H
#define KERNEL_H

#define low_word(address) (short)((address) & 0xFFFF)
#define high_word(address) (short)(((address) >> 16) & 0xFFFF)

//#define MAX_MB 128  // отображаем первые 128 МБ
#define STACK_ADDRESS 0x90000

#include <stdint.h>      // для uint32_t и других целочисленных типов
#include "draw.h"        // графические функции (draw_char, draw_string, fill_screen)
#include "isr.h"         // обработка прерываний (struct regs, idt_init, pic_init, isr_handler)
#include "syscalls.h"    // системные вызовы (SYS_* константы, sys_draw_char)

#include "use_drivers.h"

//Точка входа ядра (определена в kernel.c)
void _start(void);

void switch_to_usermode(uint32_t entry);

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t get_geode_gx1_framebuffer_address(void);

void kernel_halt_all(void);

#endif
