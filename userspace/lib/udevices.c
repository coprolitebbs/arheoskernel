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

#include "../include/udevices.h"
#include "../include/syscall.h"
#include <stddef.h>

int u_usb_count(void)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_USB_COUNT) : "memory");
    return ret;
}

int u_usb_get_dev(int idx, u_usb_dev_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_USB_GET_DEV), "b"(idx), "c"(out) : "memory");
    return ret;
}

const char* u_usb_class_name(uint8_t c)
{
    switch (c)
    {
    case 0x00:
        return "Per-Interface";
    case 0x01:
        return "Audio";
    case 0x02:
        return "CDC Communications";
    case 0x03:
        return "HID";
    case 0x05:
        return "Physical";
    case 0x06:
        return "Image";
    case 0x07:
        return "Printer";
    case 0x08:
        return "Mass Storage";
    case 0x09:
        return "Hub";
    case 0x0A:
        return "CDC-Data";
    case 0x0B:
        return "Smart Card";
    case 0x0D:
        return "Content Security";
    case 0x0E:
        return "Video";
    case 0x0F:
        return "Personal Healthcare";
    case 0x10:
        return "Audio/Video";
    case 0xDC:
        return "Diagnostic";
    case 0xE0:
        return "Wireless";
    case 0xEF:
        return "Miscellaneous";
    case 0xFE:
        return "App Specific";
    case 0xFF:
        return "Vendor Specific";
    default:
        return "Unknown";
    }
}

const char* u_usb_vendor_name(uint32_t vid)
{
    switch (vid)
    {
    case 0x46f4:
        return "QEMU";
    case 0x0627:
        return "QEMU";
    case 0x8087:
        return "Intel";
    case 0x1d6b:
        return "Linux Foundation";
    case 0x046d:
        return "Logitech";
    case 0x0781:
        return "SanDisk";
    case 0x090c:
        return "Silicon Motion";
    case 0x13fe:
        return "Phison";
    case 0x0908:
        return "A-DATA";
    case 0x058f:
        return "Alcor";
    case 0x1307:
        return "Transcend";
    case 0x0930:
        return "Toshiba";
    case 0x1058:
        return "Western Digital";
    case 0x152d:
        return "JMicron";
    case 0x0bda:
        return "Realtek";
    default:
        return NULL;
        break;
    }
}
