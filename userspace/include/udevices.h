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

#ifndef UDEVICES_H
#define UDEVICES_H

#include <stdint.h>
#include "udevices_struct.h"


//Возвращает количество USB-устройств в реестре ядра
int u_usb_count(void);

//Возвращает 0 при успехе, -1 при неверном idx
int u_usb_get_dev(int idx, u_usb_dev_t *out);

//Утилита: возвращает строковое имя класса устройства (без malloc)
const char* u_usb_class_name(uint8_t dev_class);

//Утилита: возвращает имя вендора по VID или NULL (без malloc)
const char* u_usb_vendor_name(uint32_t vid);


//PCI

//Возвращает количество PCI-устройств (ядро при первом вызове сканирует шины)
int u_pci_count(void);

//Возвращает 0 при успехе, -1 при неверном idx
int u_pci_get_dev(int idx, u_pci_dev_t *out);

//Чтение dword из конфиг-пространства устройства из реестра.
//bdf = PCI_BDF(bus,dev,func); off — смещение 0..255.
//Возвращает dword или 0xFFFFFFFF (ошибка / устройство не в реестре)
uint32_t u_pci_read_cfg(uint32_t bdf, uint8_t off);

//Утилита: имя базового класса PCI (без malloc)
const char* u_pci_class_name(uint8_t base_class);

//Утилита: «полное» имя класса с учётом подкласса (без malloc)
const char* u_pci_class_sub_name(uint8_t base_class, uint8_t subclass);

//Утилита: имя вендора по VID или NULL (без malloc)
const char* u_pci_vendor_name(uint16_t vid);

//Утилита: имя устройства по VID:DID или NULL (без malloc, только популярные)
const char* u_pci_device_name(uint16_t vid, uint16_t did);



#endif
