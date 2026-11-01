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

#endif
