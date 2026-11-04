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

#include "include/usb_hub.h"
#include "include/usb_core.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

/* GET_DESCRIPTOR(Hub) — узнать bNbrPorts */
__attribute__((noinline)) int hub_get_desc(usb_hcd_t *hcd, uint8_t addr, uint16_t max_pkt,
                        uint8_t *bNbrPorts, uint8_t *bPwrOn2PwrGood){
    uint8_t setup[8] = {
        0xA0, 0x06, 0x00, 0x29, 0x00, 0x00, 9, 0
    };
    uint8_t buf[9];
    int r = usb_hcd_control_transfer(hcd, addr, setup, buf, 9, 1, max_pkt);
    if (r != 9) return -1;
    *bNbrPorts = buf[2];
    *bPwrOn2PwrGood = buf[5];
    return 0;
}

/* SET_PORT_FEATURE(PORT_POWER) на каждый порт */
__attribute__((noinline)) int hub_power_port(usb_hcd_t *hcd, uint8_t addr, uint16_t max_pkt, uint8_t port){
    uint8_t setup[8] = {
        0x23, HUB_REQ_SET_FEATURE,
        (uint8_t)(HUB_FEAT_PORT_POWER & 0xFF),
        (uint8_t)(HUB_FEAT_PORT_POWER >> 8),
        port, 0x00, 0, 0
    };
    return usb_hcd_control_transfer(hcd, addr, setup, NULL, 0, 0, max_pkt) == 0 ? 0 : -1;
}

/* GET_PORT_STATUS — возвращает wPortStatus и wPortChange */
__attribute__((noinline)) int hub_port_status(usb_hcd_t *hcd, uint8_t addr, uint16_t max_pkt,
                           uint8_t port, uint16_t *st, uint16_t *chg){
    uint8_t setup[8] = {
        0xA3, HUB_REQ_GET_STATUS, 0, 0, port, 0x00, 4, 0
    };
    uint8_t buf[4];
    int r = usb_hcd_control_transfer(hcd, addr, setup, buf, 4, 1, max_pkt);
    if (r != 4) return -1;
    *st  = buf[0] | ((uint16_t)buf[1] << 8);
    *chg = buf[2] | ((uint16_t)buf[3] << 8);
    return 0;
}

/* SET_PORT_FEATURE(PORT_RESET) + дождаться C_PORT_RESET */
int hub_port_reset(usb_hcd_t *hcd, uint8_t addr, uint16_t max_pkt, uint8_t port){
    uint8_t setup[8] = {
        0x23, HUB_REQ_SET_FEATURE,
        (uint8_t)(HUB_FEAT_PORT_RESET & 0xFF),
        (uint8_t)(HUB_FEAT_PORT_RESET >> 8),
        port, 0x00, 0, 0
    };
    if (usb_hcd_control_transfer(hcd, addr, setup, NULL, 0, 0, max_pkt) != 0) return -1;

    for (int t = 0; t < 50; t++){
        for (volatile int i = 0; i < 200000; i++);
        uint16_t st, chg;
        if (hub_port_status(hcd, addr, max_pkt, port, &st, &chg) != 0) continue;
        if (chg & HUB_FEAT_C_PORT_RESET){
            /* CLEAR_PORT_FEATURE(C_PORT_RESET) */
            uint8_t cls[8] = {
                0x23, HUB_REQ_CLEAR_FEATURE,
                (uint8_t)(HUB_FEAT_C_PORT_RESET & 0xFF),
                (uint8_t)(HUB_FEAT_C_PORT_RESET >> 8),
                port, 0x00, 0, 0
            };
            usb_hcd_control_transfer(hcd, addr, cls, NULL, 0, 0, max_pkt);
            return 0;
        }
    }
    return -1;
}

/* Сканирует порты hub'а и вызывает callback для каждого найденного
   устройства. Callback сам делает enumerate и регистрацию. */
int usb_hub_scan(usb_hcd_t *hcd, uint8_t hub_addr, uint16_t hub_max_pkt,
                 uint8_t *next_addr,
                 int (*on_device)(usb_hcd_t *hcd, int port_idx, uint8_t new_addr,
                                  void *user),
                 void *user){
    uint8_t nbr_ports = 0, pwr_on = 0;
    if (hub_get_desc(hcd, hub_addr, hub_max_pkt, &nbr_ports, &pwr_on) != 0)
        return -1;

    for (uint8_t p = 1; p <= nbr_ports; p++){
        hub_power_port(hcd, hub_addr, hub_max_pkt, p);
    }

    /* Дать питанию подняться — pwr_on × 2 мс, минимум 100 мс */
    uint32_t delay = (pwr_on ? pwr_on : 50) * 20000;
    for (volatile uint32_t i = 0; i < delay; i++);

    int found = 0;
    for (uint8_t p = 1; p <= nbr_ports; p++){
        uint16_t st, chg;
        if (hub_port_status(hcd, hub_addr, hub_max_pkt, p, &st, &chg) != 0)
            continue;
        /* bit0 = PORT_CONNECTION */
        if (!(st & 0x0001)) continue;

        if (hub_port_reset(hcd, hub_addr, hub_max_pkt, p) != 0) continue;

        /* Дать устройству переподключиться к адресу 0 */
        for (volatile int i = 0; i < 1000000; i++);

        uint8_t dev_addr = *next_addr;
        if (on_device(hcd, (int)p, dev_addr, user) == 0){
            (*next_addr)++;
            found++;
        }
    }
    return found;
}
