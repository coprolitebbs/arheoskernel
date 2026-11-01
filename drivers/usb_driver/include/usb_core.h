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

#ifndef USB_CORE_H
#define USB_CORE_H

#include <stdint.h>
#include "usb_uhci.h"
#include "usb_hcd.h"

/* bRequest */
#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_SET_CONFIG       0x09

/* wValue high byte = descriptor type */
#define USB_DESC_DEVICE          0x01
#define USB_DESC_CONFIG          0x02
#define USB_DESC_STRING          0x03
#define USB_DESC_INTERFACE       0x04
#define USB_DESC_ENDPOINT        0x05

/* ---------------- Configuration / Interface / Endpoint ---------------- */

#define USB_CLASS_MASS_STORAGE  0x08
#define USB_SUBCLASS_SCSI       0x06
#define USB_PROTOCOL_BOT        0x50

#define USB_EP_ATTR_BULK        0x02

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

/* Разобранный Mass Storage интерфейс */
typedef struct {
    uint8_t  interface_num;
    uint8_t  bulk_in_addr;
    uint8_t  bulk_out_addr;
    uint16_t bulk_in_max_pkt;
    uint16_t bulk_out_max_pkt;
    /* Toggle хранится здесь — по одному на направление, сбрасывается в 0
       после SET_CONFIGURATION */
    uint8_t  bulk_in_toggle;
    uint8_t  bulk_out_toggle;
} usb_msc_iface_t;


/* Прототипы */
int usb_enumerate(usb_hcd_t *hcd, int port, uint8_t new_addr,
                  usb_device_desc_t *out_desc, uint8_t *out_addr);

int usb_get_config_desc(usb_hcd_t *hcd, uint8_t addr, void *buf,
                        uint16_t want_len, uint16_t max_pkt);
int usb_set_configuration(usb_hcd_t *hcd, uint8_t addr, uint8_t cfg_val);
int usb_parse_mass_storage(const void *config_buf, uint16_t total_len,
                           usb_msc_iface_t *out);

int usb_get_string_desc(usb_hcd_t *hcd, uint8_t addr, uint8_t index,
                        uint16_t lang_id, char *out, uint16_t max_len,
                        uint16_t max_pkt);

int usb_probe_device_at_addr(usb_hcd_t *hcd, int port, uint8_t addr,
                             usb_device_desc_t *out_desc);

int usb_clear_ep_halt(usb_hcd_t *hcd, uint8_t addr, uint8_t ep);

#endif
