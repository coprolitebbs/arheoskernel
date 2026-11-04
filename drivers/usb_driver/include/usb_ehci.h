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

#ifndef USB_EHCI_H
#define USB_EHCI_H

#include <stdint.h>

typedef struct {
    uint32_t mmio_base;   //физический MMIO base (BAR0)
    uint32_t op_base;     //mmio_base + CAPLENGTH (operational registers)
    uint32_t n_ports;
    int      initialized;
} ehci_hc_t;

extern ehci_hc_t ehci_hcs[4];
extern int ehci_hc_count;

//Скорость по USB-адресу: 0=FS, 1=LS, 2=HS.
//Заполняется в usb_enumerate по PSPD, читается в ehci_control_transfer
//для выбора QH_EP_SPD_* и MPS.
extern uint8_t ehci_dev_speed[128];

int ehci_probe_all(void);
int ehci_alloc_structures(void);
int ehci_init(ehci_hc_t *ehc);
int ehci_take_ports(ehci_hc_t *ehc);   //CF=1
int ehci_port_connected(ehci_hc_t *ehc, int port);
int ehci_port_reset(ehci_hc_t *ehc, int port);
uint32_t ehci_port_status(ehci_hc_t *ehc, int port);

int ehci_control_transfer(ehci_hc_t *ehc, uint8_t addr,
                          const uint8_t *setup,
                          void *buf, uint16_t len, int in, uint16_t maxpkt);
int ehci_bulk_transfer(ehci_hc_t *ehc, uint8_t addr, uint8_t ep,
                       uint16_t maxpkt, uint8_t *toggle,
                       void *buf, uint32_t len, int in);

#endif
