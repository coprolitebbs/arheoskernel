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

#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>
#include "ports_io.h"

//‘труктуры ACPI

struct RSDPtr {
    uint8_t  Signature[8];      //"RSD PTR "
    uint8_t  CheckSum;
    uint8_t  OemID[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;       //физический адрес RSDT
} __attribute__((packed));

struct FACP {
    uint8_t  Signature[4];      //"FACP"
    uint32_t Length;
    uint8_t  unneded1[40 - 8];
    uint32_t DSDT;              //физический адрес DSDT
    uint8_t  unneded2[48 - 44];
    uint32_t SMI_CMD;
    uint8_t  ACPI_ENABLE;
    uint8_t  ACPI_DISABLE;
    uint8_t  unneded3[64 - 54];
    uint32_t PM1a_CNT_BLK;
    uint32_t PM1b_CNT_BLK;
    uint8_t  unneded4[89 - 72];
    uint8_t  PM1_CNT_LEN;
} __attribute__((packed));


void beep(uint32_t count);

uint32_t enumerate_pci_ven_dev_ids(void);
void find_and_prepare_s3_trio64(void);

void acpi_init(void);
void acpi_poweroff(void);

#endif
