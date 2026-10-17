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

#ifndef DRIVER_ELF_H
#define DRIVER_ELF_H

#include "bootinfo.h"
#include "../drivers/include_drivers/drv_format.h"

//Стартовый адрес для загрузки драйверов
extern uint32_t driver_start_load_addr;

void *load_driver_elf(unsigned char *elf_image,struct boot_info *boot);

void *load_driver_elf_mem(uint32_t addr,uint32_t size,struct boot_info *boot);

void* load_driver_elf_from_disk(const char *path, struct boot_info *boot);



#endif
