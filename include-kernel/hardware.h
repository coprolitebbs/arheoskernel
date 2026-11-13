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
#include "../userspace/include/udevices_struct.h"

//Смещения в конфигурационном пространстве (заголовок типа 0x00 / 0x01)
#define PCI_CFG_VENDOR_ID   0x00
#define PCI_CFG_CMD         0x04
#define PCI_CFG_STATUS      0x06
#define PCI_CFG_REV_CLASS   0x08
#define PCI_CFG_HEADER_TYPE 0x0E
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_SUBSYSTEM   0x2C
#define PCI_CFG_ROM_BASE    0x30
#define PCI_CFG_IRQ_LINE    0x3C
#define PCI_CFG_BR_PRIMARY  0x18
#define PCI_CFG_BR_SECONDARY 0x19

#define PCI_CLASS_BRIDGE       0x06
#define PCI_SUBCLASS_PCI_BRIDGE 0x04

#define PCI_HTYPE_NORMAL      0x00
#define PCI_HTYPE_BRIDGE      0x01
#define PCI_HTYPE_CARDBUS     0x02
#define PCI_HTYPE_MULTIFUNC   0x80

//Максимум уровней вложенности мостов — защита от мусорных конфигураций
#define PCI_SCAN_MAX_DEPTH 6

//Порты PCI Configuration Mechanism #1
#define PCI_CONFIG_ADDRESS_PORT 0x0CF8
#define PCI_CONFIG_DATA_PORT    0x0CFC

//PCI_BDF()/PCI_BDF_BUS()/PCI_BDF_DEV()/PCI_BDF_FUNC() берутся из upci_struct.h

//Конфигурационное пространство: механизм #1 умеет только 0..255
#define PCI_CFG_OFF_MAX 255

//‘труктуры ACPI

struct RSDPtr
{
    uint8_t  Signature[8];      //"RSD PTR "
    uint8_t  CheckSum;
    uint8_t  OemID[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;       //физический адрес RSDT
} __attribute__((packed));

struct FACP
{
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


//Доступ к конфигурационному пространству (механизм #1)
uint32_t pci_cfg_read_dword(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset);
void     pci_cfg_write_dword(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset, uint32_t value);
uint16_t pci_cfg_read_word(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset);
uint8_t  pci_cfg_read_byte(uint32_t bus, uint32_t dev, uint32_t func, uint8_t offset);

//Сканирование шин (лениво, при первом системном вызове от userspace)
//Заполняет внутренний реестр; повторные вызовы — no-op
int  pci_scan_all(void);

//Реестр найденных устройств
int  pci_registry_count(void);
int  pci_registry_get(int idx, pci_dev_record_t *out);

//Чтение конфиг-пространства устройства из реестра (безопасно для userspace).
//Возвращает 0 при успехе, -1 если устройство не найдено или offset вне диапазона
int  pci_registry_read_cfg(uint32_t bdf, uint8_t offset, uint32_t *out_value);

#endif
