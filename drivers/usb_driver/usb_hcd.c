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

#include "include/usb_hcd.h"
#include "include/usb_uhci.h"
#include "include/ehci_priv.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

int usb_hcd_control_transfer(usb_hcd_t *hcd, uint8_t addr,
                             const uint8_t *setup,
                             void *data, uint16_t dlen,
                             int data_in, uint16_t max_pkt){
    if (!hcd) return -1;
    switch (hcd->type){
        case HCD_UHCI:
            return uhci_control_transfer((uhci_hc_t *)hcd->priv,
                                         addr, setup, data, dlen, data_in, max_pkt);
        case HCD_EHCI:
            return ehci_control_transfer((ehci_hc_t *)hcd->priv,
                                         addr, setup, data, dlen, data_in, max_pkt);
        default: return -1;
    }
}

int usb_hcd_bulk_transfer(usb_hcd_t *hcd, uint8_t addr, uint8_t ep_addr,
                          uint16_t ep_max_pkt, uint8_t *toggle,
                          void *buf, uint32_t len, int dir_in){
    if (!hcd) return -1;
    switch (hcd->type){
        case HCD_UHCI:
            return uhci_bulk_transfer((uhci_hc_t *)hcd->priv,
                                      addr, ep_addr, ep_max_pkt, toggle, buf, len, dir_in);
        case HCD_EHCI:
            return ehci_bulk_transfer((ehci_hc_t *)hcd->priv,
                                      addr, ep_addr, ep_max_pkt, toggle, buf, len, dir_in);
        default: return -1;
    }
}

int usb_hcd_reset_port(usb_hcd_t *hcd, int port){
    if (!hcd) return -1;
    switch (hcd->type){
        case HCD_UHCI: return uhci_reset_port((uhci_hc_t *)hcd->priv, port);
        case HCD_EHCI: return ehci_reset_port((ehci_hc_t *)hcd->priv, port);
        default: return -1;
    }
}

int usb_hcd_port_connected(usb_hcd_t *hcd, int port){
    if (!hcd) return 0;
    switch (hcd->type){
        case HCD_UHCI: return uhci_port_connected((uhci_hc_t *)hcd->priv, port);
        case HCD_EHCI: return ehci_port_connected((ehci_hc_t *)hcd->priv, port);
        default: return 0;
    }
}

uint16_t usb_hcd_port_status(usb_hcd_t *hcd, int port){
    if (!hcd) return 0;
    switch (hcd->type){
        case HCD_UHCI: return uhci_port_status((uhci_hc_t *)hcd->priv, port);
        case HCD_EHCI: return (uint16_t)ehci_port_status((ehci_hc_t *)hcd->priv, port);
        default: return 0;
    }
}

void usb_hcd_delay_2ms(usb_hcd_t *hcd){
    if (hcd && hcd->type == HCD_UHCI){
        uhci_hc_t *hc = (uhci_hc_t *)hcd->priv;
        for (volatile int i = 0; i < 20000; i++) inw(hc->io_base);
    } else {
        for (volatile int i = 0; i < 4000; i++) inb(0x80);   // ~2 мс на любом железе
    }
}
