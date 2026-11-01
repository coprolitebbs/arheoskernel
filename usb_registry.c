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

#include "include-kernel/usb_registry.h"
//#include "userspace/include/udevices_struct.h"
#include "include-kernel/lib.h"

static usb_dev_record_t g_usb_devs[USB_REGISTRY_MAX];
static int g_usb_devs_count = 0;

void usb_registry_clear(void){
    memset(g_usb_devs, 0, sizeof(g_usb_devs));
    g_usb_devs_count = 0;
}

int usb_registry_add(const usb_dev_record_t *rec){
    if (!rec) return -1;
    if (g_usb_devs_count >= USB_REGISTRY_MAX) return -1;
    g_usb_devs[g_usb_devs_count] = *rec;
    return g_usb_devs_count++;
}

int usb_registry_count(void){
    return g_usb_devs_count;
}

int usb_registry_get(int idx, usb_dev_record_t *out){
    if (idx < 0 || idx >= g_usb_devs_count || !out) return -1;
    *out = g_usb_devs[idx];
    return 0;
}
