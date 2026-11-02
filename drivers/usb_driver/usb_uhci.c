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

 /*
 * usb_uhci.c - UHCI Host Controller: PCI probe, reset, frame list,
 * port reset, control transfer. Этап 1: только по одному устройству
 * на root port, только control endpoint 0.
 */

#include "include/usb_uhci.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

/* ---------------- Статические UHCI-структуры ---------------- */

/* Frame List: 1024 указателя по 4 байта, физическое выравнивание 4 KB.
   В identity map нижней памяти физический адрес == виртуальный. */
uint8_t frame_list[4096] __attribute__((aligned(4096)));
uint8_t *frame_list_ptr;
uhci_qh_t control_qh __attribute__((aligned(16)));
uhci_qh_t *control_qh_ptr;
uhci_td_t control_tds[MAX_CONTROL_TDS] __attribute__((aligned(16)));
uhci_td_t *control_tds_ptr;
uint8_t setup_static[8] __attribute__((aligned(16)));
uint8_t *setup_static_ptr;

/* Bulk transfer использует отдельный QH и свой пул TD.
   Control и Bulk не конкурируют за одни и те же TD, но оба подвешиваются
   в один и тот же Frame List — одновременно они не выполняются,
   потому что вызовы синхронные. */
uhci_qh_t bulk_qh __attribute__((aligned(16)));
uhci_qh_t *bulk_qh_ptr;
uhci_td_t bulk_tds[MAX_BULK_TDS] __attribute__((aligned(16)));
uhci_td_t *bulk_tds_ptr;

/* UHCI — bus-master, ему нужен физический адрес. Stack/driver .bss
   identity-mapped (VA==PA), но kmalloc-буферы живут по VA != PA.
   Делаем walk по текущим page tables через CR3. */
uint32_t usb_virt_to_phys(uint32_t va){
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= 0xFFFFF000u;

    uint32_t *pd = (uint32_t*)cr3;
    uint32_t pde = pd[va >> 22];
    if (!(pde & 1)) return va;                 /* not present — fallback */

    uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000u);
    uint32_t pte = pt[(va >> 12) & 0x3FF];
    if (!(pte & 1)) return va;

    return (pte & 0xFFFFF000u) | (va & 0xFFF);
}

/* ---------------- PCI ---------------- */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off){
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val){
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}


/* ---------------- Probe ---------------- */

int uhci_probe(uhci_hc_t *hc){
    if (!hc) return -1;
    memset(hc, 0, sizeof(*hc));

    for (uint16_t bus = 0; bus < 256; bus++){
        for (uint8_t dev = 0; dev < 32; dev++){
            for (uint8_t func = 0; func < 8; func++){
                uint32_t id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t class = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t base_cls = (class >> 24) & 0xFF;
                uint8_t sub_cls  = (class >> 16) & 0xFF;
                uint8_t prog_if  = (class >>  8) & 0xFF;

                if (base_cls == PCI_CLASS_SERIAL_BUS &&
                    sub_cls  == PCI_SUBCLASS_USB &&
                    prog_if  == PCI_PROGIF_UHCI){

                    uint32_t bar4 = pci_read32((uint8_t)bus, dev, func, 0x20);
                    if (!(bar4 & 0x01)) continue;    /* не I/O BAR */
                    uint16_t io_base = bar4 & 0xFFFC;
                    if (io_base == 0) continue;

                    /* Включить I/O Space + Bus Master */
                    uint32_t cmd = pci_read32((uint8_t)bus, dev, func, 0x04);
                    cmd |= 0x05;
                    pci_write32((uint8_t)bus, dev, func, 0x04, cmd);

                    hc->io_base  = io_base;
                    hc->pci_bus  = (uint8_t)bus;
                    hc->pci_dev  = dev;
                    hc->pci_func = func;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ---------------- Init ---------------- */

int uhci_wait_hcreset(uhci_hc_t *hc){
    for (uint32_t i = 0; i < 100000; i++){
        if (!(inw(hc->io_base + UHCI_USBCMD) & USBCMD_HCRESET)) return 0;
    }
    return -1;
}

int uhci_init(uhci_hc_t *hc){
    if (!hc) return -1;

    /* 1) Global Reset — возвращает HC в исходное состояние */
    //outw(hc->io_base + UHCI_USBCMD, USBCMD_GRESET);
    //for (volatile int i = 0; i < 30000; i++) inw(hc->io_base + UHCI_USBCMD); /* ~10 ms */
    //outw(hc->io_base + UHCI_USBCMD, 0);

    /* 2) Host Controller Reset */
    outw(hc->io_base + UHCI_USBCMD, USBCMD_HCRESET);
    if (uhci_wait_hcreset(hc) != 0) return -2;

    /* 3) Отключить все прерывания — работаем polling'ом */
    outw(hc->io_base + UHCI_USBINTR, 0);

    /* 4) Очистить статус */
    outw(hc->io_base + UHCI_USBSTS, 0x003F);

    /* 5) Frame List — все слоты terminate (Vf=1, нет QH) */
    uint32_t *fl = (uint32_t*)frame_list_ptr;
    for (int i = 0; i < 1024; i++) fl[i] = TD_LINK_TERM;
    /* Физический адрес = виртуальный (identity map нижних 16 МБ) */
    outl(hc->io_base + UHCI_FLBASEADD, (uint32_t)frame_list_ptr);
    outw(hc->io_base + UHCI_FRNUM, 0);
    outw(hc->io_base + UHCI_SOFMOD, 64);   /* 64 байта на кадр */

    /* 6) Запуск HC */
    uint16_t cmd = inw(hc->io_base + UHCI_USBCMD);
    outw(hc->io_base + UHCI_USBCMD, cmd | USBCMD_RS);

    /* 7) Дождаться, что HC снял HCH */
    for (uint32_t i = 0; i < 100000; i++){
        if (!(inw(hc->io_base + UHCI_USBSTS) & USBSTS_HCH)) return 0;
    }
    return -3;
}

/* ---------------- Ports ---------------- */

uint16_t portsc_reg(int port){
    return (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
}

uint16_t uhci_port_status(uhci_hc_t *hc, int port){
    return inw(hc->io_base + portsc_reg(port));
}

int uhci_port_connected(uhci_hc_t *hc, int port){
    return (uhci_port_status(hc, port) & PORTSC_CCS) ? 1 : 0;
}

int uhci_reset_port(uhci_hc_t *hc, int port){
    uint16_t reg = portsc_reg(port);

    uint16_t s = inw(hc->io_base + reg);
    if (!(s & PORTSC_CCS)) return -1;    /* нет устройства */

    /* Assert: PR=1, PED=1.
       PED в записи обязателен — QEMU (и некоторые real HW) реализуют
       запись в PORTSC как полную замену, а не честный W1C. */
    outw(hc->io_base + reg, PORTSC_PR | PORTSC_PED);

    /* Держим reset ≥ 50 ms (FS; LS хватит и 10, но с запасом) */
    for (volatile int i = 0; i < 60000; i++) inw(hc->io_base + reg);

    /* Deassert: читаем СВЕЖИЙ PORTSC (HW мог уже поставить PED),
       снимаем PR, PED форсируем =1. */
    uint16_t cur = inw(hc->io_base + reg);
    outw(hc->io_base + reg, (cur & ~PORTSC_PR) | PORTSC_PED);

    /* Дать порту стабилизироваться ≥ 10 ms */
    for (volatile int i = 0; i < 120000; i++) inw(hc->io_base + reg);

    uint16_t fin = inw(hc->io_base + reg);
    outw(hc->io_base + reg, fin | PORTSC_CSC | PORTSC_PEDC | PORTSC_PED);

    /* Финальная проверка */
    for (uint32_t i = 0; i < 200000; i++){
        s = inw(hc->io_base + reg);
        if (s & PORTSC_PED) return 0;
    }
    return -2;
}

/* ---------------- Control transfer ---------------- */

int uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr,
                          const uint8_t *setup_pkt,
                          void *data_buf, uint16_t data_len,
                          int data_in, uint16_t max_pkt){

    if (!hc || !setup_pkt) return -1;
    if (max_pkt == 0 || max_pkt > 64) max_pkt = 8;

    /* Сколько TD понадобится под data stage (по одному на пакет) */
    uint32_t num_data = 0;
    int has_data = (data_len > 0 && data_buf != NULL);
    if (has_data){
        num_data = (data_len + max_pkt - 1) / max_pkt;
        if (num_data > MAX_CONTROL_TDS - 2){
            num_data = MAX_CONTROL_TDS - 2;   /* обрезаем, если не влезает */
        }
    }
    uint32_t total_tds = 1 + num_data + 1;    /* setup + data + status */

    /* Setup packet копируем в статику (identity-map) */
    for (int i = 0; i < 8; i++) setup_static_ptr[i] = setup_pkt[i];

    uhci_td_t *tds = control_tds_ptr;
    uint8_t   *buf = (uint8_t *)data_buf;

    /* --- TD[0]: SETUP, DATA0, 8 байт --- */
    tds[0].link   = ((uint32_t)&tds[1]) & ~0x0Fu;
    tds[0].status = TD_CTRL_ACTIVE;
    tds[0].token  = ((uint32_t)PID_SETUP << TD_TOKEN_PID_SHIFT)
                  | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                  | (0u << TD_TOKEN_EP_SHIFT)
                  | 0u                                              /* DATA0 */
                  | ((uint32_t)(8 - 1) << TD_TOKEN_LEN_SHIFT);
    tds[0].buffer = (uint32_t)setup_static_ptr;

    /* --- TD[1..num_data]: Data stage, чанки по max_pkt ---
       Toggle в data stage начинается с DATA1 и чередуется:
       i=0 → DATA1, i=1 → DATA0, i=2 → DATA1, ... */
    for (uint32_t i = 0; i < num_data; i++){
        uint32_t idx   = 1 + i;
        uint32_t off   = i * max_pkt;
        uint32_t chunk = (data_len - off > max_pkt) ? max_pkt
                                                    : (data_len - off);
        uint32_t toggle = ((i & 1) == 0) ? TD_TOKEN_TOGGLE : 0u;
        uint32_t next   = (i + 1 < num_data)
                            ? ((uint32_t)&tds[idx + 1]) & ~0x0Fu
                            : ((uint32_t)&tds[1 + num_data]) & ~0x0Fu;

        tds[idx].link   = next;
        tds[idx].status = TD_CTRL_ACTIVE;
        tds[idx].token  = ((uint32_t)(data_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT)
                        | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                        | (0u << TD_TOKEN_EP_SHIFT)
                        | toggle
                        | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        tds[idx].buffer = usb_virt_to_phys((uint32_t)(buf + off));
    }

    /* --- Status TD: направление противоположно data, всегда DATA1, 0 байт --- */
    uint32_t sidx       = 1 + num_data;
    uint32_t status_pid = has_data ? (data_in ? PID_OUT : PID_IN) : PID_IN;

    tds[sidx].link   = TD_LINK_TERM;
    tds[sidx].status = TD_CTRL_ACTIVE | TD_CTRL_IOC;
    tds[sidx].token  = (status_pid << TD_TOKEN_PID_SHIFT)
                     | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                     | (0u << TD_TOKEN_EP_SHIFT)
                     | TD_TOKEN_TOGGLE
                     | ((uint32_t)0x7FF << TD_TOKEN_LEN_SHIFT);
    tds[sidx].buffer = (uint32_t)setup_static_ptr;

    /* --- QH --- */
    control_qh_ptr->head    = ((uint32_t)&tds[0]) & ~0x0Fu;
    control_qh_ptr->element = TD_LINK_TERM;

    uint32_t qh_phys = (uint32_t)control_qh_ptr;
    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = (qh_phys & ~0x0Fu) | TD_LINK_Q;

    /* --- Ожидание --- */
    uint32_t timeout = 10000000;
    int result = -3;
    while (timeout--){
        uint32_t st = tds[sidx].status;
        if (!(st & TD_CTRL_ACTIVE)){
            uint32_t bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR |
                           TD_CTRL_BABBLE  | TD_CTRL_CRCTIMEO;
            if (st & bad){ result = -2; break; }
            /* Проверим, что data TDs тоже прошли без ошибок */
            int data_ok = 1;
            for (uint32_t i = 0; i < num_data; i++){
                if (tds[1 + i].status & bad){ data_ok = 0; break; }
            }
            result = data_ok ? (int)data_len : -2;
            break;
        }
    }

    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = TD_LINK_TERM;

    (void)total_tds;
    return result;
}

int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr,
                       uint16_t ep_max_pkt, uint8_t *toggle,
                       void *buf, uint32_t len, int dir_in){

    if (!hc || !buf || len == 0 || !toggle) return -1;
    if (ep_max_pkt == 0 || ep_max_pkt > 1024) ep_max_pkt = 64;

    uint32_t num_tds = (len + ep_max_pkt - 1) / ep_max_pkt;
    if (num_tds > MAX_BULK_TDS){
        num_tds = MAX_BULK_TDS;
        len = num_tds * ep_max_pkt;    /* режем запрос до влезающего */
    }

    uint8_t ep_num = ep_addr & 0x0F;
    uint8_t cur_toggle = *toggle ? 1 : 0;
    uint8_t *b = (uint8_t *)buf;
    uhci_td_t *tds = bulk_tds_ptr;

    for (uint32_t i = 0; i < num_tds; i++){
        uint32_t off   = i * ep_max_pkt;
        uint32_t chunk = (len - off > ep_max_pkt) ? ep_max_pkt : (len - off);
        uint32_t toggle_bit = cur_toggle ? TD_TOKEN_TOGGLE : 0u;
        uint32_t next = (i + 1 < num_tds)
                          ? ((uint32_t)&tds[i + 1]) & ~0x0Fu
                          : TD_LINK_TERM;

        tds[i].link   = next;
        tds[i].status = TD_CTRL_ACTIVE | (i == num_tds - 1 ? TD_CTRL_IOC : 0u);
        tds[i].token  = ((uint32_t)(dir_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT)
                      | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                      | ((uint32_t)ep_num << TD_TOKEN_EP_SHIFT)
                      | toggle_bit
                      | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        tds[i].buffer = usb_virt_to_phys((uint32_t)(b + off));

        cur_toggle ^= 1;
    }

    bulk_qh_ptr->head    = ((uint32_t)&tds[0]) & ~0x0Fu;
    bulk_qh_ptr->element = TD_LINK_TERM;

    uint32_t qh_phys = (uint32_t)bulk_qh_ptr;
    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = (qh_phys & ~0x0Fu) | TD_LINK_Q;

    uint32_t timeout = 5000000;
    int result = -3;
    while (timeout--){
        uint32_t st = tds[num_tds - 1].status;
        if (!(st & TD_CTRL_ACTIVE)){
            uint32_t bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR |
                       TD_CTRL_BABBLE  | TD_CTRL_CRCTIMEO;
            int all_ok = 1;
            for (uint32_t k = 0; k < num_tds; k++){
                if (tds[k].status & (bad | TD_CTRL_ACTIVE)){
                    all_ok = 0;
                    break;
                }
            }
            if (all_ok){ *toggle = cur_toggle; result = (int)len; }
            else       { result = -2; }
            break;
        }
    }

    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = TD_LINK_TERM;

    return result;
}


void uhci_register_ptrs(){
    frame_list_ptr = frame_list;
    control_qh_ptr = &control_qh;
    control_tds_ptr = control_tds;
    setup_static_ptr = setup_static;
    bulk_qh_ptr     = &bulk_qh;
    bulk_tds_ptr    = bulk_tds;
}