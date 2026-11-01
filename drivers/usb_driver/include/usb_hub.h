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

#ifndef USB_HUB_H
#define USB_HUB_H

#include <stdint.h>
#include "usb_uhci.h"
#include "usb_hcd.h"

#define HUB_CLASS 0x09
#define HUB_REQ_GET_STATUS       0x00
#define HUB_REQ_CLEAR_FEATURE    0x01
#define HUB_REQ_SET_FEATURE      0x03
#define HUB_FEAT_PORT_RESET      0x0004
#define HUB_FEAT_PORT_POWER      0x0008
#define HUB_FEAT_C_PORT_RESET    0x0010



int usb_hub_scan(usb_hcd_t *hcd, uint8_t hub_addr, uint16_t hub_max_pkt,
                 uint8_t *next_addr,
                 int (*on_device)(usb_hcd_t *hcd, int port_idx, uint8_t new_addr,
                                  void *user),
                 void *user);

#endif
