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

#ifndef VBE_RM_H
#define VBE_RM_H

#include <stdint.h>
#include "../drivers/include_drivers/drv_format.h"
#include "../drivers/include_drivers/driver_graphic_layer.h"


//Область тримплина в младшей памяти. Адреса обязаны совпадать с
//vbe_rm_blob.asm и gdt.c (дескрипторы с базой 0x4000 на индексах 128/129).
//Зона 0x4000..0x47FF не занята никем: BDA заканчивается на 0x4FF,
//bootinfo с 0x5000, stage2 с 0x7E00, pmm никогда не раздаёт память ниже 1 МБ.
#define VBE_RM_BLOB_ADDR 0x4000
#define VBE_RM_BLOB_MAX  0x200
#define VBE_RM_REQ_ADDR  0x4200
#define VBE_RM_RES_ADDR  0x4220
#define VBE_RM_INFO_ADDR 0x4300
#define VBE_RM_INFO_SIZE 0x400

//Набор регистров для вызова BIOS int 10h через тримплин.
//es на входе - СЕГМЕНТНЫЙ адрес буфера структуры (phys >> 4), 0 если не нужен;
//на выходе - фактический ES после BIOS-вызова.

//Копирование машинного блоба на VBE_RM_BLOB_ADDR. Вызывается ядром один раз
//на старте (до включения пейджинга - запись идёт по физическим адресам).
void vbe_rm_init(void);

//Вызов функции int 10h (VBE) через возврат в реальный режим.
//Заполняете поля regs, результат читаете там же после возврата.
//Возвращает 0, если AX == 0x004F (успех VBE), иначе -1.
//Не вызывать из обработчиков прерываний (маскируются IRQ, стек уходит в RM).
int vbe_rm_exec(vbe_rm_regs_t *regs);

//Буфер 1 КБ под структуры VBE (VbeInfoBlock 512 байт, ModeInfoBlock 256 байт).
//Лежит по физическому адресу VBE_RM_INFO_ADDR, identity-mapped, читается напрямую.
uint8_t *vbe_rm_info_buf(void);

#endif
