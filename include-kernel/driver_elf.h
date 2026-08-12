#ifndef DRIVER_ELF_H
#define DRIVER_ELF_H

#include "bootinfo.h"
#include "../drivers/include_drivers/drv_format.h"

//Стартовый адрес для загрузки драйверов
extern uint32_t driver_start_load_addr;

void *load_driver_elf(unsigned char *elf_image,struct boot_info *boot);

void *load_driver_elf_mem(uint32_t addr,uint32_t size,struct boot_info *boot);


#endif
