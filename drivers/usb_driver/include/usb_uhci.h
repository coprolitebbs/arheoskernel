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

//Критическая секция: запрещает вытеснение задачи на время BOT-последовательности / транзакции
//Ядро передаёт драйверу указатель на kernel_schedule_lock через api->schedule_lock_ptr;
//если его нет (драйвер крутится до multitasking) — no-op. cli/sti не используются,
//т.к. прерывания в ядре разворачиваются в yield (см. syscalls.c)
#ifdef __cplusplus
#define USB_SCHED_LOCK(p)
#define USB_SCHED_UNLOCK(p)
#else
#define USB_SCHED_LOCK(p)  do { if (p) *(volatile uint8_t*)(p) = 1; } while(0)
#define USB_SCHED_UNLOCK(p) do { if (p) *(volatile uint8_t*)(p) = 0; } while(0)
#endif

//PCI
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB     0x03
#define PCI_PROGIF_UHCI      0x00
#define PCI_PROGIF_OHCI      0x10
#define PCI_PROGIF_EHCI      0x20

//UHCI регистры (offset от BAR4)
#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

//USBCMD
#define USBCMD_RS       (1 << 0)
#define USBCMD_HCRESET  (1 << 1)
#define USBCMD_GRESET   (1 << 2)   //Global Reset: reset всех портов и PHY
#define USBCMD_EGSM     (1 << 3)   //Enter Global Suspend Mode
#define USBCMD_CF       (1 << 6)   //Configure Flag: 0 = legacy (BIOS) mode

//USBSTS
#define USBSTS_USBINT   (1 << 0)
#define USBSTS_ERROR    (1 << 1)
#define USBSTS_RESET    (1 << 2)
#define USBSTS_RESUME   (1 << 3)
#define USBSTS_HCH      (1 << 5)   //HC Halted
#define USBSTS_HSE      (1 << 6)   //Host System Error (PCI abort и т.п.)

//PORTSC
#define PORTSC_CCS      (1 << 0)   //Current Connect Status
#define PORTSC_CSC      (1 << 1)   //Connect Status Change
#define PORTSC_PED      (1 << 2)   //Port Enabled
#define PORTSC_PEDC     (1 << 3)   //Port Enable/Disable Change
#define PORTSC_LS       (3 << 4)
#define PORTSC_LSDA     (1 << 8)   //Low Speed Device Attached
#define PORTSC_PR       (1 << 9)   //Port Reset
#define PORTSC_SUSPEND  (1 << 12)
#define PORTSC_POCI     (1 << 7)   //OverCurrent Indicator
#define PORTSC_DS       (1 << 16)  //Debug Mode Support
#define PORTSC_DR        (1 << 17) //Debug Mode Software Entry
#define PORTSC_SDE      (1 << 22)  //Soft Disconnect Enable (ICH/PIIX4)
#define PORTSC_W1C      (PORTSC_CSC | PORTSC_PEDC | (1 << 4) | (1 << 5) | \
                         (1 << 6) | (1 << 7) | (1 << 10) | (1 << 11) | \
                         (1 << 13) | (1 << 14) | (1 << 15))

//TD Link Pointer
#define TD_LINK_TERM    (1 << 0)   //Terminate — валидного указателя нет
#define TD_LINK_Q       (1 << 1)   //QH, не TD
#define TD_LINK_VF      (1 << 2)   //Vertical First — depth-first, нам не нужен

//TD Status (bits 16..31, т.к. младшие 16 = actual length)
#define TD_CTRL_ACTIVE      (1 << 23)
#define TD_CTRL_STALLED     (1 << 22)
#define TD_CTRL_DBUFERR     (1 << 21)
#define TD_CTRL_BABBLE      (1 << 20)
#define TD_CTRL_NAK         (1 << 19)
#define TD_CTRL_CRCTIMEO    (1 << 18)
#define TD_CTRL_BITSTUFF    (1 << 17)
#define TD_CTRL_IOC         (1 << 24)
#define TD_CTRL_LS          (1 << 26)
#define TD_CTRL_SPD         (1 << 29)
//Error Counter (bits 27-28): сколько раз HC повторит транзакцию при
//NAK/CRC/BitStuff перед тем как retire TD. 3 = 4 попытки
//После retire с NAK драйвер должен сам решить: retry или ошибка
#define TD_CTRL_MAXERR_SHIFT 27
#define TD_CTRL_MAXERR_3    (3u << TD_CTRL_MAXERR_SHIFT)


//Token
#define TD_TOKEN_PID_SHIFT  0
#define TD_TOKEN_ADDR_SHIFT 8
#define TD_TOKEN_EP_SHIFT   15
#define TD_TOKEN_TOGGLE     (1 << 19)
#define TD_TOKEN_LEN_SHIFT  21

#define PID_SETUP   0x2D
#define PID_IN      0x69
#define PID_OUT     0xE1

#define MAX_CONTROL_TDS   16
#define MAX_BULK_TDS      64

//#define PAGE_SIZE 4096

//Максимум UHCI-контроллеров (PCI class 0x0C/0x03/prog-if 0)
//Физические машины часто имеют 2 контроллера по 2 порта (ICH: USB#1/#2),
//поэтому одного hc на весь драйвер не хватает
#define UHCI_MAX_HC       4

//Сколько control-TD может одновременно висеть в расписании HC
//Пока идёт enumeration устройства на порту A, HC продолжает крутить port-TD на порту B
//(hot-patch статуса). Насколько успевает застрять наша очередь — ограничено; MAX_CONTROL_TDS с запасом
#define UHCI_CTRL_RING    64
#define UHCI_BULK_RING    128


#define UHCI_TICKS_PER_SEC  100
#define UHCI_CTRL_TIMEOUT_SEC 2
#define UHCI_BULK_TIMEOUT_SEC 5


#define UHCI_MAX_CONTROLLERS 8

//Структуры UHCI (все выровнены на 16)
typedef struct
{
    volatile uint32_t link;
    volatile uint32_t status;   //bits 0..15 = actual length, 16..31 = status
    volatile uint32_t token;
    volatile uint32_t buffer;
} __attribute__((packed, aligned(16))) uhci_td_t;

typedef struct
{
    volatile uint32_t head;     //первый TD цепочки
    volatile uint32_t element;  //следующий QH
} __attribute__((packed, aligned(16))) uhci_qh_t;

typedef struct
{
    uint16_t io_base;
    uint8_t  index;       //slot 0..UHCI_MAX_HC-1
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;
    uint8_t  ports;       //число root-портов (обычно 2)
    uint8_t  valid;       //слот заполнен
    uint8_t  running;     //HC инициализирован и RS=1

    //Планировщик
    uhci_qh_t *port_qh;   //&port_qhs[index][0] — порт 0
    uhci_qh_t *port_qh1;  //&port_qhs[index][1] — порт 1
    uhci_qh_t *ctrl_qh;
    uhci_qh_t *bulk_qh;
    uint8_t   *frame_list;
    uint32_t   fl_phys;

    //Указатели на статические кольца TD этого HC
    uhci_td_t *ctrl_ring;
    uhci_td_t *bulk_ring;
} uhci_hc_t;

//Статические буферы HC: выделяются под максимум контроллеров
//uhci_hc_ptrs[] указывает на i-й элемент — так указатели остаются валидными при повторном probe()/init()
extern uhci_hc_t *uhci_hc_ptrs[UHCI_MAX_HC];
extern int        uhci_hc_count;

extern uint8_t *frame_list_ptr;
extern uhci_qh_t *control_qh_ptr;
extern uhci_td_t *control_tds_ptr;

extern uint8_t uhci_dev_is_ls[128];

//PCI helpers
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

//UHCI API
int  uhci_probe_all(void);             //найти ВСЕ UHCI-контроллеры в PCI
int  uhci_init_all(void);              //проинициализировать все найденные HC
int  uhci_init(uhci_hc_t *hc);         //сбросить и запустить один HC
int  uhci_port_connected(uhci_hc_t *hc, int port);
int  uhci_reset_port(uhci_hc_t *hc, int port);
uint16_t uhci_port_status(uhci_hc_t *hc, int port);
int  uhci_hc_index(uhci_hc_t *hc);

//Инкрементальное планирование: все root-порты всех контроллеров постоянно висят в Frame List,
//HC сам опрашивает их каждый кадр (~1 мс). Устройства (control/bulk очереди)
//добавляются/снимаются динамически через schedule_* / deschedule_*
void uhci_ports_schedule_all(void);
void uhci_ports_deschedule_all(void);

//Проверки выравнивания структур расписания для ВСЕХ HC (frame list — 4K, QH/TD — 16).
//Возвращает 0 при успехе
int  uhci_check_alignment(void);

//Сколько портов у контроллера (UHCI spec — 2)
static inline int uhci_hc_ports(uhci_hc_t *hc)
{
    return hc && hc->ports ? hc->ports : 2;
}

//Hot-patch port-TD (вернуть ACTIVE "дожжённым" TD).
//Вызывается из основного цикла опроса портов примерно раз за кадр
void uhci_ports_poll_refresh(void);

//Включить/выключить конкретную очередь (ctrl/bulk) в кадрах HC
void uhci_ctrl_queue_enable(uhci_hc_t *hc, int enable);
void uhci_bulk_queue_enable(uhci_hc_t *hc, int enable);

//Hard reset порта без участия расписания (PR=1, PED=0, suspend off)
void uhci_port_reset_hard(uhci_hc_t *hc, int port);

//Снять порт с suspend (некоторые контроллеры/BIOS вешают порт в SUSPEND)
void uhci_port_resume(uhci_hc_t *hc, int port);

//Control transfer
int  uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr,
                           const uint8_t *setup_pkt,
                           void *data_buf, uint16_t data_len,
                           int data_in, uint16_t max_pkt);

//Bulk transfer (endpoint != 0). Требует явный toggle — он хранится на стороне вызывающего в
//usb_msc_iface_t и переключается после каждого успешного пакета
int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr,
                       uint16_t ep_max_pkt, uint8_t *toggle,
                       void *buf, uint32_t len, int dir_in);

//Проверка состояния HC во время ожидания TD: HSE, Error, зависание
int uhci_hc_healthy(uhci_hc_t *hc);
//int uhci_probe(uhci_hc_t *hc);
int uhci_probe_all(void);


//Отладочные счётчики (пишет kernel via api->debugN)
extern uint32_t uhci_dbg_hse;
extern uint32_t uhci_dbg_err;
extern uint32_t uhci_dbg_timeout;

extern uhci_hc_t uhci_hcs[8];
extern int uhci_hc_count;

int uhci_check_alignment(void);

//void uhci_register_ptrs();
//DMA-структуры контроллера (Frame List, QH, TD-пулы, setup-буфер)
//Вызываются из device_usb_start(): к этому моменту dma_alloc_ptr уже установлен ядром
//(см. use_drivers.c). uhci_register_ptrs() удалён — больше нет статических массивов,
//все структуры выделяются динамически
int  uhci_alloc_structures(void);
void uhci_free_structures(void);

#endif
