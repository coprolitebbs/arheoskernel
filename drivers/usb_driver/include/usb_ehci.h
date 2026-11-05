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


//EHCI регистры (смещения от operational base = mmio + CAPLENGTH)
#define EHCI_USBCMD        0x00
#define EHCI_USBSTS        0x04
#define EHCI_USBINTR       0x08
#define EHCI_FRINDEX       0x0C
#define EHCI_CTRLDSSEG     0x10
#define EHCI_PERIODICLIST  0x14
#define EHCI_ASYNCLISTADDR 0x18
#define EHCI_CONFIGFLAG    0x40
#define EHCI_PORTSC(p)     (0x44 + ((p)-1)*4)   //p=1..N

//Регистры совместимости (offset от mmio_base)
#define EHCI_CAPLENGTH     0x00
#define EHCI_HCSPARAMS     0x04
#define EHCI_HCCPARAMS     0x08

//USBCMD
#define CMD_RUN            (1u << 0)
#define CMD_HCRESET        (1u << 1)
#define CMD_PSE            (1u << 4)    //Periodic Schedule Enable
#define CMD_ASE            (1u << 5)    //Async Schedule Enable
#define CMD_ITC_SHIFT      16           //Interrupt Threshold Control

//USBSTS
#define STS_HCHALTED       (1u << 12)
#define STS_PSS            (1u << 14)   //Periodic Schedule Status
#define STS_ASS            (1u << 15)   //Async Schedule Status

//CONFIGFLAG
#define CFG_ROUTE_ALL      1            //CF=1: все порты к EHCI

//PORTSC
#define PSC_CCS            (1u << 0)
#define PSC_CSC            (1u << 1)
#define PSC_PED            (1u << 2)
#define PSC_PEDC           (1u << 3)
#define PSC_PR             (1u << 8)
#define PSC_PP             (1u << 12)   //Port Power
#define PSC_PO             (1u << 13)   //Port Owner (1=companion, 0=EHCI)
#define PSC_PSPD_SHIFT     26           //PSPD bits 27:26: 0=FS,1=LS,2=HS
#define PSC_W1C            (PSC_CSC | PSC_PEDC)   //change-биты W1C

//qTD Token — раскладка по Linux ehci.h / EHCI spec Table 3-5
#define QTD_TOGGLE         (1u << 31)   //DCR: при D=1 в QH — DATA1 если 1
#define QTD_BYTES(t)       (((t) >> 16) & 0x7FFFu)  //остаток байт к передаче
#define QTD_IOC            (1u << 15)
#define QTD_CERR(t)        (((t) >> 10) & 0x3u)
#define QTD_CERR_SET       (3u << 10)   //3 попытки на qTD (Linux EHCI_TUNE_CERR)
#define QTD_PID(t)         (((t) >> 8) & 0x3u)
#define QTD_PID_OUT        (0u << 8)
#define QTD_PID_IN         (1u << 8)
#define QTD_PID_SETUP      (2u << 8)
#define QTD_STS_ACTIVE     (1u << 7)
#define QTD_STS_HALT       (1u << 6)
#define QTD_STS_DBE        (1u << 5)
#define QTD_STS_BABBLE     (1u << 4)
#define QTD_STS_XACT       (1u << 3)
#define QTD_STS_MMF        (1u << 2)
#define QTD_STS_STS        (1u << 1)

//Ссылки на QH: Type-биты bits 2:1 = 01b (Linux Q_TYPE_QH = 1<<1)
#define Q_TYPE_QH          (1u << 1)
#define LINK_T             1u           //Terminate

//QH Endpoint Characteristics (Word 1) — по Linux ehci.h
#define QH_EP_RL(n)        (((uint32_t)(n) & 0xFu) << 28) //NAK reload
#define QH_EP_C            (1u << 27)   //FS/LS control endpoint
#define QH_EP_MAXPKT(m)    (((uint32_t)(m) & 0x7FFu) << 16)
#define QH_EP_H            (1u << 15)   //head of reclamation list
#define QH_EP_D            (1u << 14)   //Data toggle control: 1 = из DCR qTD
#define QH_EP_SPD_HS       (2u << 12)   //EPS bits 14:12
#define QH_EP_SPD_LS       (1u << 12)
#define QH_EP_SPD_FS       (0u << 12)
#define QH_EP_NUM(n)       (((uint32_t)(n) & 0xFu) << 8)  //endpoint bits 11:8
#define QH_EP_ADDR(a)      ((uint32_t)(a) & 0x7Fu)        //address bits 6:0

//QH Endpoint Capabilities (Word 2)
#define QH_CAP_MULT(n)     (((uint32_t)(n) & 0x3u) << 30) //Mult (1..3, не 0)

#define EHCI_CTRL_QH_POOL   8
#define EHCI_CTRL_QTD_COUNT 4           //SETUP + DATA + STATUS с запасом
#define EHCI_BULK_QTD_COUNT 64          //по 4KB = до 256KB за вызов
#define EHCI_BULK_POOL_PAGES 10         //40KB bounce-пул (MAX_BULK_TDS*512 = 32KB данных)
#define EHCI_BULK_POOL_SIZE  (EHCI_BULK_POOL_PAGES * 4096)
#define EHCI_CTL_DATA_MAX    3584       //данные control через 4KB bounce
#define EHCI_QTD_PAGE        4096

typedef struct
{
    uint32_t mmio_base;   //физический MMIO base (BAR0)
    uint32_t mmio_size;   //размер BAR0 по PCI size-probing (обычно 0x400)
    uint32_t op_base;     //mmio_base + CAPLENGTH (operational registers)
    uint32_t n_ports;
    int      initialized;
    uint8_t  pci_bus;     //адрес функции — нужен для BIOS handoff
    uint8_t  pci_dev;     //(USBLEGSUP живёт в PCI config space)
    uint8_t  pci_func;
} ehci_hc_t;

//Развёртка qTD — 32-битная раскладка EHCI spec 3.5 (Linux struct ehci_qtd,
//QEMU EHCIqtd). Hi-слова НЕ вперемешку с указателями, а одним блоком
//в конце. В 32-битном режиме HC читает ровно 32 байта:
//next(0x00), alt(0x04), token(0x08), buf[0..4](0x0C..0x1B).
typedef struct
{
    volatile uint32_t next;    //0x00 Next qTD Pointer (bit0 = Terminate)
    volatile uint32_t alt;     //0x04 Alternate Next qTD Pointer
    volatile uint32_t token;   //0x08 qTD Token
    uint32_t buf[5];           //0x0C..0x1B Buffer Pointers 0..4
    uint32_t buf_hi[5];        //0x20..0x2F — только 64-bit HC, всегда 0
} __attribute__((packed, aligned(32))) ehci_qtd_t;          // 52 bytes

//Развёртка QH — 32-битная раскладка EHCI spec 3.6 (Linux struct ehci_qh_hw,
//QEMU EHCIqh). В 32-битном режиме HC читает ровно 48 байт:
//horiz(0x00), ep_char(0x04), ep_cap(0x08), cur_qtd(0x0C),
//next_qtd(0x10), alt_qtd(0x14), token(0x18), buf[0..4](0x1C..0x2C).
typedef struct
{
    volatile uint32_t horiz;    //0x00 Next QH Pointer (|Q_TYPE_QH)
    volatile uint32_t ep_char;  //0x04 Endpoint Characteristics
    volatile uint32_t ep_cap;   //0x08 Endpoint Capabilities
    volatile uint32_t cur_qtd;  //0x0C Current qTD Pointer
    volatile uint32_t next_qtd; //0x10 Overlay: Next qTD Pointer
    volatile uint32_t alt_qtd;  //0x14 Overlay: Alternate Next qTD
    volatile uint32_t token;    //0x18 Overlay: qTD Token
    uint32_t buf[5];            //0x1C..0x2C Overlay: Buffer Pointers
    uint32_t buf_hi[5];         //0x30..0x3F — только 64-bit HC, всегда 0
} __attribute__((packed, aligned(32))) ehci_qh_t;   //68 bytes

//extern usb_stack_driver_api_t *api_ptr;
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
