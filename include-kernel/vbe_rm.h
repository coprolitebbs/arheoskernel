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
