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
#define UHCI_MAX_HCS 8

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
#define UHCI_PORTSC3    0x14   /* есть не во всех HC; чтение вернёт 0xFFFF/0 */
#define UHCI_PORTSC4    0x16

/* USBSTS bits (UHCI Rev1.1 §5.2) */
#define USBSTS_USBINT   (1 << 0)
#define USBSTS_ERROR    (1 << 1)
#define USBSTS_RESET    (1 << 2)
#define USBSTS_RESUME   (1 << 3)   /* Resume Detected — порт висит в suspend */
#define USBSTS_HCH      (1 << 5)   /* HC Halted */

/* USBCMD */
#define USBCMD_RS       (1 << 0)
#define USBCMD_HCRESET  (1 << 1)
#define USBCMD_GRESET   (1 << 2)
#define USBCMD_EGSM     (1 << 3)   /* Enter Global Suspend Mode */
#define USBCMD_CF       (1 << 6)

/* PORTSC (UHCI Spec Rev 1.1 §5.4 / Intel ICH datasheet):
   bit0  CCS  — Current Connect Status (RO)
   bit1  CSC  — Connect Status Change  (W1C)
   bit2  PED  — Port Enable/Disable    (R/W: 1 = порт включён)
   bit3  PEDC — Port En/Dis Change     (W1C)
   bit4  LS   — Low Speed              (RO legacy)
   bit5  LSE  — Low Speed Enable       (RO legacy)
   bit6  OCI  — Over-current Indicator (RO)
   bit7  OCIC — Over-current Change    (W1C)
   bit8  LSDA — Low-Speed Device Attached (RO)
   bit9  PR   — Port Reset             (ставится софтом, АВТОСБРАСЫВАЕТСЯ самим
                                       HC через ~10 мс — UHCI spec §4.1!)
   bit12 SUSP — Suspend of port        (R/W)
   КРИТИЧЕСКИЙ БАГ ПРЕДЫДУЩИХ ВЕРСИЙ: очистка change-битов делалась записью
   вида "ps | CSC | PEDC", где ps содержал НЕОБНУЛЁННЫЙ CCS (bit0). На ICH
   запись 1 в RO-бит CCS при активном порте трактуется как смена состояния
   порта: ровно поэтому в логе CCS(0x001) и CSC(0x002) гасли одновременно
   ("pre=91 -> fin=288": сброс даже не начинался, устройство «пропало»).
   Теперь любое значение PORTSC собирается строго из R/W-бит (PED/PR/SUSP),
   а W1C-единицы добавляются отдельной маской. */
#define PORTSC_CCS      (1u << 0)   /* Current Connect Status (RO) */
#define PORTSC_CSC      (1u << 1)   /* Connect Status Change (W1C) */
#define PORTSC_PED      (1u << 2)   /* Port Enable/Disable (R/W) */
#define PORTSC_PEDC     (1u << 3)   /* Port Enable/Disable Change (W1C) */
#define PORTSC_LS       (1u << 4)   /* Low Speed (RO) */
#define PORTSC_LSE      (1u << 5)   /* Low Speed Enable (RO) */
#define PORTSC_OCI      (1u << 6)   /* Over-current Indicator (RO) */
#define PORTSC_OCIC     (1u << 7)   /* Over-current Change (W1C) */
#define PORTSC_LSDA     (1u << 8)   /* Low Speed Device Attached (RO) */
#define PORTSC_PR       (1u << 9)   /* Port Reset (auto-clear by HC) */
#define PORTSC_SUSPEND  (1u << 12)  /* Suspend (R/W) */

/* Только R/W-биты PORTSC — из них собираем записываемое состояние. */
#define PORTSC_RW_MASK  (PORTSC_PED | PORTSC_PR | PORTSC_SUSPEND)
/* Все W1C-биты одной маской: OR к значению для очистки change-флагов. */
#define PORTSC_W1C_ALL  (PORTSC_CSC | PORTSC_PEDC | PORTSC_OCIC)

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
/* SPD = bits 29:28. ВНИМАНИЕ: бит 29 определён как TD_CTRL_SPD — это ОШИБКА
   маски, оставшаяся с прошлых ревизий: 1<<29 попадает в поле SPD только как
   старший бит; использовать TD_CTRL_SPD как "флаг" нельзя (он затирает SPD).
   Все места кода используют явные TD_CTRL_SPD_FS/LS и маску ниже. */
#define TD_CTRL_SPD         (1u << 29)
#define TD_CTRL_C_ERR_SHIFT 26
#define TD_CTRL_C_ERR_NONE  (0u << 26)
#define TD_CTRL_C_ERR_3     (3u << 26)   /* 11b = 3 попытки HC сам (как в uhci-hcd Linux) */
#define TD_CTRL_C_ERR_MASK  TD_CTRL_C_ERR_3
/* SPD (bits 29:28): скорость, с которой HC ОБРАЩАЕТСЯ к устройству.
   00 = Full-Speed, 01 = Low-Speed, 10 = Full-Speed через Low-Speed hub.
   КРИТИЧНО для CF-18/ICH6: значение 11b зарезервировано; если оно случайно
   окажется в статусе TD при записи, контроллер ICH может вести себя некор-
   ректно. При инициализации TD всегда пишем явный SPD=00 (LS-устройства на
   root-hub — только клавиатуры; LS флаг ставится отдельно битом 25). */
#define TD_CTRL_SPD_FS      (0u << 28)
#define TD_CTRL_SPD_MASK    (3u << 28)
#define TD_CTRL_LS          (1u << 25)   /* Low Speed Device Attached */
#define TD_CTRL_IOC         (1u << 24)   /* Interrupt on Complete */
#define TD_CTRL_ACTIVE      (1u << 23)
#define TD_CTRL_STALLED     (1u << 22)
#define TD_CTRL_DBUFERR     (1u << 21)
#define TD_CTRL_BABBLE      (1u << 20)
#define TD_CTRL_NAK         (1u << 19)
#define TD_CTRL_CRCTIMEO    (1u << 18)
#define TD_CTRL_BITSTUFF    (1u << 17)
#define TD_CTRL_PIDERR      (1u << 16)   /* bit16: received PID не распознан / ответа нет */
/* Маска извлечения "received PID" из status-слова TD (UHCI Rev1.1 §4.3.3,
   QEMU hw/usb/hcd-uhci.c делает ровно также): bits 15:8 в word1 статуса. */
#define TD_STATUS_PID_SHIFT 8
#define TD_STATUS_PID_MASK  (0xFFu << TD_STATUS_PID_SHIFT)

/* Token (UHCI Spec Rev 1.1 §4.3.5):
   bits 0-7 PID | 8-14 ADDR(7b) | 15-18 ENDP(4b) | 19 DATA0/1
   | 20-30 LEN (0x7FF = max, т.е. поле занимает БИТЫ 20..30!)
   ЭТАЛОН (hw/usb/hcd-uhci.c Linux, uhci_append_pid_token):
   token = (len-1)<<20 | ep<<15 | addr<<8 | pid;
   GET_DESCRIPTOR(8) на addr0: 0x0080072D.
   КРИТИЧНО: компилятор обязан вычислять ((len-1) << TD_TOKEN_LEN_SHIFT)
   ПРИ ВЫЧИСЛЕНИИ — для длины используется только макрос TD_TOKEN_SET_LEN,
   который всегда сдвигает на 20 бит независимо от константы выше. */
#define TD_TOKEN_PID_SHIFT  0
#define TD_TOKEN_ADDR_SHIFT 8
#define TD_TOKEN_EP_SHIFT   15
#define TD_TOKEN_TOGGLE     (1u << 19)
#define TD_TOKEN_LEN_SHIFT  20        // было 21 — на один бит левее, чем надо!
#define TD_TOKEN_LEN_MASK   (0x7FFu << TD_TOKEN_LEN_SHIFT)
/* Единственный способ положить длину в токен: макрос раскрывается на месте
   использования, поэтому пересобирается всегда вместе с вызывающим файлом. */
#define TD_TOKEN_SET_LEN(tokexpr, len_minus_1)  ((tokexpr) | ((uint32_t)(len_minus_1) << 20))

#define TD_CTRL_C_ERR_1     (1u << 26)
#define TD_CTRL_C_ERR_2     (2u << 26)
/* Считанные из TD статус-слова ошибки (биты 17..22): все кроме STALLED —
   временные (NAK-подобные), их надо ретраить программно; STALLED — фатальная. */
#define TD_ERR_ANY          (TD_CTRL_STALLED | TD_CTRL_DBUFERR | TD_CTRL_BABBLE | \
                             TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF)

/* Полные байты PID КАК ИХ ГЕНЕРИРУЕТ ХОСТ НА ПРОВОДЕ (UHCI Spec Rev 1.1
   §4.3.5 / USB 1.1 Table 8-1): PID = 4-bit tag, повторенный инверсным
   дубликатом в старших битах (LSB first):
     SETUP = 0101b -> 0x2D | IN = 1001b -> 0x69 | OUT = 0001b -> 0xE1
     DATA0 = 1100b -> 0xC3 | DATA1 = 1011b -> 0x4B
   В TD token поля PID/DATA0/1 задаются именно этими значениями (так же,
   как кладет их в память Linux uhci-hcd и что ожидает модель QEMU
   hw/usb/hcd-uhci.c). Значения ниже — ЭТАЛОННЫЕ; если после сборки в
   логе «TD token» окажется их дополнение (~0x2D=0xD2 и т.п.) — значит
   где-то в цепочке применяется ложная инверсия, и именно она даёт
   статусы вида 0xC4000000/0xD8000000 (транзакция не принята устройством)
   при полностью исправном порте (CCS=1 PED=1). */
#define PID_SETUP   0x2D
#define PID_IN      0x69
#define PID_OUT     0xE1
#define PID_DATA0   0xC3
#define PID_DATA1   0x4B
/* PID ERR (0xD2): кладётся HC в поле "error PID"/bit7 статуса TD, когда НИКАКОЙ
   ответный PID не получен (device не отвечает / порты в suspend). Отличие от
   CRCTIMEO-битов: биты 18/17 при этом МОЛЧАТ (QEMU uhci_write_td_status так и
   делает) — поэтому их надо читать отдельно, а error-PID сравнивать с 0x5D. */
#define PID_ERR     0xD2

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
    /* Диагностика ПОСЛЕДНЕГО заведомо-холостого транзакта ("ping" на адресе,
       где устройства нет). Если ping завершается мгновенно с PID ERR +
       Timeouts, а рабочий GET_DESCRIPTOR висит ACTIVE до таймаута — значит
       HC физически общивает ДРУГОЙ порт (на ICH6 флешка сидит на портах
       companion-контроллера dev 23 func 1, а не dev 29 func 0): */
    uint32_t dbg_ping_status;            // status TD ping-цепочки целиком
    uint32_t dbg_ping_spins;             // итерации ожидания ping (малы = HC жив)
    int      dbg_ping_result;            // код возврата ping
    /* Порт, на котором HC физически общивается с текущим устройством
       (заполняется в uhci_reset_port после успешного PED). 0xFF = не задан.
       Нужен для корректного resume-кода перед каждой транзакцией: раньше
       будились порты 0 и 1 ВСЕГДА, а suspend активного порта мог остаться
       незамеченным на третьем/четвёртом root-hub контроллере. */
    uint8_t  active_port;
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
/* Точная задержка в МИКРОсекундах (TSC-калибровка). Нужна для корректного
   ожидания авто-снятия PR: UHCI держит reset ровно ~10 мс, и polling шагом
   по 2 мс легко «проскакивает» момент. */
void       uhci_udelay(uint32_t us);
/* Диагностика root-hub: печатает USBCMD/USBSTS/PORTSC1/PORTSC2/FRNUM +
   расшифровку битов порта. Вызывается на каждой ключевой фазе сброса. */
void       uhci_dump_ports(uhci_hc_t *hc, const char *tag);

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
