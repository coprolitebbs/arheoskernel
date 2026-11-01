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


#ifndef USB_REGISTRY_H
#define USB_REGISTRY_H

#include <stdint.h>
//#include "../userspace/include/udevices_struct.h"
#include "../drivers/include_drivers/drv_format.h"

#define USB_REGISTRY_MAX 8

void usb_registry_clear(void);

int  usb_registry_add(const usb_dev_record_t *rec);

int  usb_registry_count(void);
int  usb_registry_get(int idx, usb_dev_record_t *out);

#endif
