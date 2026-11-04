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

#ifndef USB_HCD_H
#define USB_HCD_H

#include <stdint.h>

typedef enum {
    HCD_NONE = 0,
    HCD_UHCI = 1,
    HCD_EHCI = 2
} hcd_type_t;

typedef struct {
    hcd_type_t type;
    void      *priv;
} usb_hcd_t;

/* Транспортный уровень. Возвращают то же, что uhci_*_transfer. */
int usb_hcd_control_transfer(usb_hcd_t *hcd, uint8_t addr,
                             const uint8_t *setup_pkt,
                             void *data_buf, uint16_t data_len,
                             int data_in, uint16_t max_pkt);

int usb_hcd_bulk_transfer(usb_hcd_t *hcd, uint8_t addr, uint8_t ep_addr,
                          uint16_t ep_max_pkt, uint8_t *toggle,
                          void *buf, uint32_t len, int dir_in);

int      usb_hcd_reset_port    (usb_hcd_t *hcd, int port);
int      usb_hcd_port_connected(usb_hcd_t *hcd, int port);
uint16_t usb_hcd_port_status   (usb_hcd_t *hcd, int port);

/* Универсальная пауза «2 ms» — UHCI делает это через inw(io_base),
   EHCI — busy-wait. В usb_enumerate используется в SET_ADDRESS. */
void usb_hcd_delay_2ms(usb_hcd_t *hcd);

#endif
