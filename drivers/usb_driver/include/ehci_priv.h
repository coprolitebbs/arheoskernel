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

#ifndef EHCI_PRIV_H
#define EHCI_PRIV_H

#include <stdint.h>

/* ---------- Capability registers (offset от BAR0) ---------- */
#define EHCI_CAP_CAPLENGTH     0x00   /* u8 CAPLENGTH, u8 reserved, u16 HCIVERSION */
#define EHCI_CAP_HCSPARAMS     0x04   /* u32 */
#define EHCI_CAP_HCCPARAMS     0x08   /* u32 */

/* ---------- Operational registers (offset от BAR0 + CAPLENGTH) ---------- */
#define EHCI_OP_USBCMD         0x00
#define EHCI_OP_USBSTS         0x04
#define EHCI_OP_USBINTR        0x08
#define EHCI_OP_FRINDEX        0x0C
#define EHCI_OP_CTRLDSSEG      0x10
#define EHCI_OP_PERIODICLIST   0x14
#define EHCI_OP_ASYNCLIST      0x18
#define EHCI_OP_CONFIGFLAG     0x40
#define EHCI_OP_PORTSC         0x44   /* PORTSC[n] = 0x44 + n*4 */

/* ---------- USBCMD bits ---------- */
#define EHCI_CMD_RS            0x00000001u
#define EHCI_CMD_HCRESET       0x00000002u
#define EHCI_CMD_FLS_MASK      0x0000000Cu
#define EHCI_CMD_PSE           0x00000010u
#define EHCI_CMD_ASE           0x00000020u

/* ---------- USBSTS bits ---------- */
#define EHCI_STS_USBINT        0x00000001u
#define EHCI_STS_USBERRINT     0x00000002u
#define EHCI_STS_PCD           0x00000004u
#define EHCI_STS_HSE           0x00000010u
#define EHCI_STS_HCH           0x00001000u

/* ---------- PORTSC bits ---------- */
#define EHCI_PORTSC_CCS        0x00000001u
#define EHCI_PORTSC_CSC        0x00000002u
#define EHCI_PORTSC_PE         0x00000004u
#define EHCI_PORTSC_PEC        0x00000008u
#define EHCI_PORTSC_RESUME     0x00000040u
#define EHCI_PORTSC_SUSPEND    0x00000080u
#define EHCI_PORTSC_PR         0x00000100u
#define EHCI_PORTSC_LINE       0x00000C00u
#define EHCI_PORTSC_PP         0x00001000u
#define EHCI_PORTSC_PO         0x00002000u

#define EHCI_MAX_PORTS         16

/* ---------- qTD Token fields ---------- */
#define EHCI_QTD_PID_OUT        (0u << 8)
#define EHCI_QTD_PID_IN         (1u << 8)
#define EHCI_QTD_PID_SETUP      (2u << 8)
#define EHCI_QTD_CERR(n)        (((n) & 3u) << 1)
#define EHCI_QTD_IOC            (1u << 15)
#define EHCI_QTD_ACTIVE         (1u << 7)
#define EHCI_QTD_HALTED         (1u << 6)
#define EHCI_QTD_BUFERR         (1u << 5)
#define EHCI_QTD_BABBLE         (1u << 4)
#define EHCI_QTD_XACTERR        (1u << 3)
//#define EHCI_QTD_MISSED         (1u << 2)
#define EHCI_QTD_DT             (1u << 31)
#define EHCI_QTD_LEN(n)         (((uint32_t)(((n) > 0) ? ((n) - 1) : 0) & 0x7FFFu) << 16)
#define EHCI_QTD_ERR_MASK       (EHCI_QTD_HALTED | EHCI_QTD_BUFERR | \
                                 EHCI_QTD_BABBLE | EHCI_QTD_XACTERR)

/* ---------- QH ep_cap1 fields ---------- */
#define EHCI_QH_RL(n)           (((uint32_t)((n) & 0xFu)) << 28)   /* NAK Counter Load */
#define EHCI_QH_C               (1u << 27)   /* Control endpoint flag */
#define EHCI_QH_MAXPKT(n)       (((uint32_t)((n) & 0x7FFu)) << 16)
#define EHCI_QH_HEAD            (1u << 15)   /* H — head of reclamation list */
#define EHCI_QH_DTC             (1u << 14)   /* DT control (1 = use qTD's DT) */
#define EHCI_QH_EPS_HS          (2u << 12)   /* Endpoint speed: 2 = high-speed */
#define EHCI_QH_EP(n)           (((uint32_t)((n) & 0xFu)) << 8)
#define EHCI_QH_ADDR(n)         ((uint32_t)((n) & 0x7Fu))

//---------- qTD (Queue Element Transfer Descriptor) 32 байта, aligned 32 ----------
typedef struct {
    volatile uint32_t next;       //указатель на следующий qTD | 0 (bit0=0 для qTD)
    volatile uint32_t alt_next;   //альтернативный указатель (для коротких пакетов)
    volatile uint32_t token;      //Token: PID, длина, toggle, статус
    volatile uint32_t buf[5];     //5 x 32-bit физических указателей
} __attribute__((packed, aligned(32))) ehci_qtd_t;

//---------- QH (Queue Head) 48 байт, aligned 32 ----------
typedef struct {
    volatile uint32_t horiz;          /* Horizontal link pointer */
    volatile uint32_t ep_cap1;        /* Endpoint characteristics dword 1 */
    volatile uint32_t ep_cap2;        /* Endpoint characteristics dword 2 */
    volatile uint32_t cur_qtd;        /* Current qTD (overlay) */
    volatile uint32_t overlay_next;   /* Overlay next qTD */
    volatile uint32_t overlay_alt;    /* Overlay alt-next qTD */
    volatile uint32_t overlay_token;  /* Overlay token */
    volatile uint32_t overlay_buf[5]; /* Overlay buffers */
} __attribute__((packed, aligned(32))) ehci_qh_t;

typedef struct {
    uint32_t           bar0;          //MMIO base
    uint32_t           bar0_size;     //сколько замаплено (>=0x1000)
    uint8_t            pci_bus;
    uint8_t            pci_dev;
    uint8_t            pci_func;      //D29:F?, обычно 7 на ICH4
    uint8_t            cap_length;    //0x20 на ICH4
    uint16_t           hci_version;   //0x0100
    uint32_t           hcs_params;
    uint32_t           hcc_params;
    uint8_t            n_ports;
    uint8_t            current_port_speed;  // 0=FS, 1=LS, 2=HS, 3=reserved (из PSPD)
    volatile uint32_t *op;            //BAR0 + cap_length
} ehci_hc_t;

void ehci_wr(volatile uint32_t *r, uint32_t v);
uint32_t ehci_rd(volatile uint32_t *r);


/* Probe: найти EHCI на PCI, включить Memory Space + Bus Master, вернуть BAR0 */
int  ehci_probe(ehci_hc_t *hc);

/* BIOS -> OS handoff (EHCI spec 1.0 §5.2.7) */
int  ehci_release_from_bios(ehci_hc_t *hc);

/* Init: HCRESET, очистка USBSTS, CONFIGFLAG.CF=1, читает cap_length/n_ports.
   BAR0 должен быть уже замаплен вызывающим. */
int  ehci_init(ehci_hc_t *hc);

uint32_t ehci_port_status(ehci_hc_t *hc, int port);
int      ehci_port_connected(ehci_hc_t *hc, int port);

/* Этапы 3-4: пока заглушки, возвращают -1 */
int  ehci_control_transfer(ehci_hc_t *hc, uint8_t addr,
                           const uint8_t *setup, void *data,
                           uint16_t dlen, int data_in, uint16_t max_pkt);
int  ehci_bulk_transfer(ehci_hc_t *hc, uint8_t addr, uint8_t ep_addr,
                        uint16_t ep_max, uint8_t *toggle,
                        void *buf, uint32_t len, int dir_in);
int  ehci_reset_port(ehci_hc_t *hc, int port);

//Инициализация async schedule: ASYNCLISTADDR, USBCMD.ASE=1, USBCMD.RS=1
int  ehci_start_async(ehci_hc_t *hc);

//Control transfer. setup — 8 байт, data/data_len — data stage.
int  ehci_control_transfer(ehci_hc_t *hc, uint8_t addr,
                           const uint8_t *setup, void *data,
                           uint16_t dlen, int data_in, uint16_t max_pkt);

void ehci_register_ptrs(void);

#endif
