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

#ifndef USB_UHCI_H
#define USB_UHCI_H

#include <stdint.h>

/* ---------------- PCI ---------------- */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* Сколько UHCI-контроллеров поддерживаем (ICH6: dev 29 funcs 0..2 + dev 23 companion) */
#define UHCI_MAX_HCS 4

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB     0x03
#define PCI_PROGIF_UHCI      0x00
#define PCI_PROGIF_OHCI      0x10
#define PCI_PROGIF_EHCI      0x20

/* ---------------- UHCI регистры (offset от BAR4) ---------------- */
#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

/* USBCMD */
#define USBCMD_RS       (1 << 0)
#define USBCMD_HCRESET  (1 << 1)
#define USBCMD_GRESET   (1 << 2)
#define USBCMD_CF       (1 << 6)

/* USBSTS */
#define USBSTS_USBINT   (1 << 0)
#define USBSTS_ERROR    (1 << 1)
#define USBSTS_RESET    (1 << 2)
#define USBSTS_HCH      (1 << 5)   /* HC Halted */

/* PORTSC */
#define PORTSC_CCS      (1 << 0)   /* Current Connect Status */
#define PORTSC_CSC      (1 << 1)   /* Connect Status Change */
#define PORTSC_PED      (1 << 2)   /* Port Enabled */
#define PORTSC_PEDC     (1 << 3)   /* Port Enable/Disable Change */
#define PORTSC_LS       (3 << 4)
#define PORTSC_LSDA     (1 << 8)   /* Low Speed Device Attached */
#define PORTSC_PR       (1 << 9)   /* Port Reset */
#define PORTSC_SUSPEND  (1 << 12)

/* TD Link Pointer */
#define TD_LINK_TERM    (1 << 0)   /* Terminate — валидного указателя нет */
#define TD_LINK_Q       (1 << 1)   /* QH, не TD */
#define TD_LINK_VF      (1 << 2)   /* Vertical First — depth-first, нам не нужен */

/* TD Status (bits 16..31, т.к. младшие 16 = actual length).
   Раскладка по UHCI Spec Rev 1.1 §4.3.3 (Точный!):
     bit31 ISP  bit30 ISC  bit29-28 SPD  bit27-26 C_ERR(2b)
     bit25 LS   bit24 IOC  bit23 Active
     bit22 Stalled  bit21 DBufErr  bit20 Babble  bit19 NAK
     bit18 CRC/Time-out  bit17 BitStuff  bit16 PacketId(err) */
#define TD_CTRL_ISP         (1u << 31)
#define TD_CTRL_ISC         (1u << 30)
#define TD_CTRL_SPD         (1u << 29)
#define TD_CTRL_C_ERR_SHIFT 26
#define TD_CTRL_C_ERR_NONE  (0u << 26)
#define TD_CTRL_C_ERR_3     (3u << 26)   /* 11b = 3 попытки HC сам (как в uhci-hcd Linux) */
#define TD_CTRL_C_ERR_MASK  TD_CTRL_C_ERR_3
#define TD_CTRL_LS          (1u << 25)   /* Low Speed Device Attached */
#define TD_CTRL_IOC         (1u << 24)   /* Interrupt on Complete */
#define TD_CTRL_ACTIVE      (1u << 23)
#define TD_CTRL_STALLED     (1u << 22)
#define TD_CTRL_DBUFERR     (1u << 21)
#define TD_CTRL_BABBLE      (1u << 20)
#define TD_CTRL_NAK         (1u << 19)
#define TD_CTRL_CRCTIMEO    (1u << 18)
#define TD_CTRL_BITSTUFF    (1u << 17)

/* Token (UHCI Spec Rev 1.1 §4.3.5):
   bits 0-7 PID | 8-14 ADDR(7b) | 15-18 ENDP(4b) | 19 DATA0/1
   | 20-30 LEN (0x7FF = max, т.е. поле занимает БИТЫ 20..30!) */
#define TD_TOKEN_PID_SHIFT  0
#define TD_TOKEN_ADDR_SHIFT 8
#define TD_TOKEN_EP_SHIFT   15
#define TD_TOKEN_TOGGLE     (1u << 19)
#define TD_TOKEN_LEN_SHIFT  20        // было 21 — на один бит левее, чем надо!
#define TD_TOKEN_LEN_MASK   (0x7FFu << TD_TOKEN_LEN_SHIFT)

#define PID_SETUP   0x2D
#define PID_IN      0x69
#define PID_OUT     0xE1

#define MAX_CONTROL_TDS   16
#define MAX_BULK_TDS      64

#define PORTSC_W (PORTSC_CSC | PORTSC_PEDC | PORTSC_PR | PORTSC_SUSPEND)

/* ---------------- Структуры UHCI (все выровнены на 16) ---------------- */
/* ВАЖНО: поля объявлены как uint32_t (НЕ volatile). TD — это normal memory
   для процессора и coherent memory для DMA-агента. Compiler barrier
   ("memory" clobber) обеспечивает порядок записи до/после запуска HC.
   volatile здесь только мешает оптимизациям и не даёт никаких гарантий DMA. */
typedef struct {
    uint32_t link;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
} __attribute__((aligned(16))) uhci_td_t;

typedef struct {
    uint32_t head;
    uint32_t element;
} __attribute__((aligned(16))) uhci_qh_t;

typedef struct {
    uint16_t io_base;
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;
    uint8_t  current_device_low_speed;   // 1, если устройство на текущем адресе — LS
    /* Диагностика последнего control/bulk транзакта (для лога загрузки): */
    uint32_t dbg_td0_status;             // status первого TD (SETUP stage) после транзакта
    uint32_t dbg_td0_token;              // токен SETUP-TD (проверка ADDR/PID/LEN)
    uint32_t dbg_last_frame;             // FRNUM в момент завершения/таймаута
    uint32_t dbg_sts;                    // USBSTS в момент завершения/таймаута
    uint32_t dbg_portsc;                 // PORTSC активного порта в момент транзакта
    uint32_t dbg_swstamp;                // программная "метка времени": сколько итераций
                                         // ожидания потребовалось (0..timeout). Если равно
                                         // timeout+1 — очередь вообще не двигалась (HC dead).
} uhci_hc_t;

typedef struct {
    uhci_qh_t qh;           // Сам QH
    uhci_td_t dummy_td;     // Dummy TD для этой очереди
    // Можно добавить сюда указатель на "хвост" для быстрого добавления
    uhci_td_t *tail_td;     // Указатель на последний TD в цепочке (обычно dummy)
} uhci_queue_t;

extern uint32_t *frame_list_ptr;
extern uhci_qh_t *control_qh_ptr;
extern uhci_td_t *control_tds_ptr;
extern uint8_t *setup_static_ptr;

extern uhci_qh_t *bulk_qh_ptr;
extern uhci_td_t *bulk_tds_ptr;

// Глобальные очереди
extern uhci_queue_t g_control_queue;
extern uhci_queue_t g_bulk_queue;

/* Синхронизация DMA-памяти.
   ВАЖНО: на i386 без PAE кэш линейно адресуем — bus-master DMA когерентен
   с кэшем, инвалидация НЕ требуется. wbinvd же — тяжёлая операция: на
   ноутбуках она порою обрамляется SMI (System Management Interrupt), который
   может «съедать» миллисекунды и срывать тайминги USB-транзактов. Поэтому
   перед записью дескрипторов достаточно compiler barrier, а wbinvd оставлен
   как явный аварийный инструмент (см. dbg-переменную uhci_force_wbinvd). */
static inline void usb_dma_sync_before(void) {
    __asm__ __volatile__("" ::: "memory");
}
static inline void usb_dma_sync_after(void) {
    __asm__ __volatile__("" ::: "memory");
}

extern int uhci_force_wbinvd;   /* =1 включает режим тотального wbinvd (для A/B теста) */

uint32_t usb_virt_to_phys(uint32_t va);

/* PCI helpers */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

/* UHCI API */
int  uhci_probe(uhci_hc_t *hc);        /* найти UHCI в PCI, заполнить hc */
void uhci_init_queues(void);
int  uhci_init(uhci_hc_t *hc);         /* сбросить и запустить HC */
int  uhci_port_connected(uhci_hc_t *hc, int port);
/* КОРРЕКТНЫЙ reset по UHCI spec (аналг uhci_hub.c::uhci_port_reset в Linux):
   PR=1 на >=10ms, затем PR=0 и ждём PED=1 (до ~250мс). Возврат -2 означает,
   что порт не смог включиться (нет устройства / device D+ pull-up пропал). */
int  uhci_reset_port(uhci_hc_t *hc, int port);
uint16_t uhci_port_status(uhci_hc_t *hc, int port);

/* Глобальный «активный» хост-контроллер: все TD/QH — единая статическая
   память, поэтому одновременно крутить транзакты может только один HC.
   uhci_select_host() делает текущим контроллер hc (и сбрасывает очереди). */
void uhci_select_host(uhci_hc_t *hc);
extern uhci_hc_t *g_uhci_active;

/* Реестр найденных UHCI-контроллеров (живёт в usb_uhci.c) */
int        uhci_add_host(uint16_t io_base, uint8_t bus, uint8_t dev, uint8_t func);
uhci_hc_t *uhci_get_host(int idx);
int        uhci_get_host_count(void);
void       uhci_reset_host_list(void);
void       uhci_mdelay(uint32_t ms);

/* Control transfer */
int  uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr,
                           const uint8_t *setup_pkt,
                           void *data_buf, uint16_t data_len,
                           int data_in, uint16_t max_pkt);

void uhci_register_ptrs(void);

/* Bulk transfer (endpoint != 0). Требует явный toggle — он хранится
   на стороне вызывающего в usb_msc_iface_t и переключается после каждого
   успешного пакета. */
int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr,
                       uint16_t ep_max_pkt, uint8_t *toggle,
                       void *buf, uint32_t len, int dir_in);

#endif
