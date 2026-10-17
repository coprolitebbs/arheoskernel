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
