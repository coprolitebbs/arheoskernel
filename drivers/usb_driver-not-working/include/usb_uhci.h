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

/* Число root-портов на ОДИН UHCI HC. По spec — до 2; на ICH6 каждая UHCI
   функция = ровно 2 порта, в QEMU piix3-usb-uhci — тоже 2. Константа
   единая для probe/init/reset/enumeration, чтобы нигде не осталось
   расхождения «сканируем 4, инициализируем 2». */
#define UHCI_ROOT_PORTS 2

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
/* КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ РАСКЛАДКИ (перепроверено по QEMU hw/usb/hcd-uhci.c:
   UHCI_TD_STALL=(1<<22), UHCI_PIDERR=(1<<16), UHCI_CRC_TMO=(1<<18),
   UHCI_BITSTUFF=(1<<17) — это БИТЫ ВСЕГО 32-битного status-слова TD, а не
   «word1». Младшие 16 бит слова занимают actual length (bits 0..9) и
   RECEIVED PID (bits 10..15, 6 бит = инвертированный tag принятого пакета).
   Прежние TD_STATUS_PID_SHIFT=8 / MASK=0xFF<<8 захватывали actual length —
   на УСПЕШНОМ TD (pid_bits==0, len>0) uhci_td_is_err_pid() ложно возвращал
   истину, и каждая успешная транзакция браковалась кодом -9. Именно поэтому
   «лог не изменился»: GET_DESC(8) r=-1 при TD0=d400000 был самообманом
   драйвера, а не отказом устройства. */
#define TD_ACTLEN_MASK      0x3FFu          /* bits 0..9  */
#define TD_RECV_PID_SHIFT   10              /* bits 10..15 */
#define TD_RECV_PID_MASK    (0x3Fu << TD_RECV_PID_SHIFT)
/* Инвертированные 6-битные теги PID, как их кладёт HC/QEMU (uhci_decode_pid):
   SETUP=0x0A IN=0x06 OUT=0x0E DATA0=0x0C DATA1=0x05 MDATA=0x07 ERR=0x0D */
#define TD_PID_ERR_INV      0x0Du
#define TD_PID_NONE         0x00u           /* ответа не было вовсе */

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

/* ==================== РАдикаЛЬНАЯ ПОЧИНКА PID (реальная причина всех -12) ====
   Прежние константы (SETUP=0x2D, IN=0x69, OUT=0xE1) были НЕ полными wire-PID,
   а 4-битными тегами с ОДНИМ инверсным дубликатом. UHCI Spec Rev 1.1 §4.3.5 и
   QEMU hw/usb/hcd-uhci.c (uhci_token_pid / UHCI_PID_*), Linux uhci-hcd
   (uhci_append_pid_token -> usb_get_pid()): в TOKEN TD кладётся PID, где КАЖДЫЙ
   бит 4-битного тега продублирован инверсией, LSB-first:
       tag nibble T=t3t2t1t0 -> byte = t0 ~t0 t1 ~t1 t2 ~t2 t3 ~t3
   Правильные эталоны:
       SETUP tag=0x5 -> 0x0D | IN  tag=0x9 -> 0x91 | OUT tag=0x1 -> 0x5D
       DATA0 tag=0x3 -> 0x33 | DATA1 tag=0xB -> 0xDB | PING tag=0x0 -> 0x0D(?)
   Спецификация USB 1.1 Table 8-1 подтверждает: SETUP=0x0D, IN=0x91, OUT=0x5D,
   DATA0=0x33, DATA1=0xDB.
   СТАРЫЕ ЗНАЧЕНИЯ (0x2D/0x69/0xE1) — это НЕ валидные wire-PID: хост подал на
   шину мусор, ни одно устройство пакет не распознalo, ответного PID нет ->
   received-PID=NONE(0x00), TD завершается с CRCTIMEO|BITSTUFF (0xD800000) либо
   PIDERR|CRCTIMEO|BITSTUFF (0xD400000). Это объясняет ВСЕ наблюдения сразу:
     - identical лог в QEMU и CF-18 (шина не приняла пакет на обеих машинах);
     - отсутствие изменений после починки детекции ошибок (сам токен был мусором);
     - VID=bad3 (enumeration никогда не начинался корректно).
   Ниже — ЭТАЛОННЫЕ значения. Если в логе «TD0 tok» для addr=0 снова НЕ равно
   0x0080000D — сборка драйвера в памяти ДРУГАЯ (см. UHCI_CODE_REV). */
#define PID_SETUP   0x0Du
#define PID_IN      0x91u
#define PID_OUT     0x5Du
#define PID_DATA0   0x33u
#define PID_DATA1   0xDBu
#define PID_PING    0x00u
/* PID ERR (0xD2): кладётся HC в поле "error PID"/bit7 статуса TD, когда НИКАКОЙ
   ответный PID не получен (device не отвечает / порты в suspend). Отличие от
   CRCTIMEO-битов: биты 18/17 при этом МОЛЧАТ (QEMU uhci_write_td_status так и
   делает) — поэтому их надо читать отдельно, а error-PID сравнивать с 0x5D. */
#define PID_ERR     0xD2u

#define MAX_CONTROL_TDS   16
#define MAX_BULK_TDS      64

#define PORTSC_W (PORTSC_CSC | PORTSC_PEDC | PORTSC_PR | PORTSC_SUSPEND)

/* ---------------- Структуры UHCI (все выровнены на 16) ---------------- */
/* ВАЖНО: поля объявлены как uint32_t (НЕ volatile). TD — это normal memory
   для процессора и coherent memory для DMA-агента. Compiler barrier
   ("memory" clobber) обеспечивает порядок записи до/после запуска HC.
   volatile здесь только мешает оптимизациям и не даёт никаких гарантий DMA. */
typedef struct {
    volatile uint32_t link;
    volatile uint32_t status;   /* ОБЯЗАТЕЛЬНО volatile: HC пишет в это слово
        по DMA асинхронно. Без volatile цикл ожидания компилируется в
        «бесконечное чтение одного значения из регистра CPU» — на реальном
        железе (CF-18) оно так и не обновляется, и каждая транзакция
        завершается ложным TIMEOUT при реально уже отработанном TD. В QEMU
        эффект маскировался таймингом эмуляции. */
    volatile uint32_t token;
    volatile uint32_t buffer;
} __attribute__((aligned(16))) uhci_td_t;

typedef struct {
    volatile uint32_t head;
    volatile uint32_t element;  /* volatile — см. комментарий к TD.status */
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
    /* Диагностика ПОСЛЕДНЕГО заведомо-холостого транзакта (см. ниже):
       раньше здесь был «ping» на несуществующем адресе 15 — но по UHCI spec
       PID ERR при отсутствии ответа НЕ выставляется (биты 16..22 молчат),
       поэтому ping давал ложный «успех len=0» и ничего не различал. Удалён
       как метод; поля сохранены только для бинарной совместимости структуры
       с уже собранными образами ядра. */
    uint32_t dbg_ping_status;            // (не используется, reserved)
    uint32_t dbg_ping_spins;             // (не используется, reserved)
    int      dbg_ping_result;            // (не используется, reserved)
    /* Порт, на котором HC физически общивается с текущим устройством
       (заполняется в uhci_reset_port после успешного PED). 0xFF = не задан.
       Нужен для корректного resume-кода перед каждой транзакцией: раньше
       будились порты 0 и 1 ВСЕГДА, а suspend активного порта мог остаться
       незамеченным на третьем/четвёртом root-hub контроллере. */
    uint8_t  active_port;
    /* Счётчик срабатываний стража живости uhci_ensure_running(). Если в логе
       финальной ошибки restarts велико — HC гаснет регулярно (SMI/BIOS), и
       это подтверждает правильность выбранной защиты. */
    uint32_t dbg_restarts;
    /* Счётчик радикальных восстановлений (uhci_hard_recovery, A118). */
    uint32_t dbg_recoveries;
} uhci_hc_t;

typedef struct {
    uhci_qh_t qh;           // Сам QH
    uhci_td_t dummy_td;     // Dummy TD для этой очереди
    // Можно добавить сюда указатель на "хвост" для быстрого добавления
    uhci_td_t *tail_td;     // Указатель на последний TD в цепочке (обычно dummy)
} uhci_queue_t;

extern volatile uint32_t *frame_list_ptr;
extern uhci_qh_t *control_qh_ptr;
extern uhci_td_t *control_tds_ptr;
extern uint8_t *setup_static_ptr;

extern uhci_qh_t *bulk_qh_ptr;
extern uhci_td_t *bulk_tds_ptr;

// Глобальные очереди
extern uhci_queue_t g_control_queue;
extern uhci_queue_t g_bulk_queue;

/* Синхронизация DMA-памяти. ПРЕДЫДУЩЕЕ ПРЕДПОЛОЖЕНИЕ («на i386 без PAE
   bus-master DMA когерентен с кэшем») — ЛОЖЬ в общем виде. Кэш линейно
   адресуем, но это НЕ делает DMA когерентным: UHCI — bus-master, его записи
   идут в память, МИМУЯ кэш CPU. Если строка TD/QH остаётся в модифицированном
   состоянии в кэше CPU, HC читает из DRAM СТАРУЮ копию, а его обновления
   статуса CPU не видит вообще никогда. На CF-18 (Pentium-M, 32KB L1 WB) это и
   есть главный отказ: TD навсегда Active -> ложный TIMEOUT. На QEMU эффект
   маскировался тем, что эмуляция трогала память напрямую.
   Поэтому ОБЯЗАТЕЛЬНО wbinvd (write-back + invalidate) ПЕРЕДАЁМ HC дескрипторы
   и ПОСЛЕ завершения транзакта. Риск SMI при wbinvd меньше риска чтения
   мусора; для кадровых таймингов UHCI это безопасно (мы и так поллим). */
static inline void usb_dma_sync_before(void) {
    __asm__ __volatile__("wbinvd" ::: "memory");
}
static inline void usb_dma_sync_after(void) {
    __asm__ __volatile__("wbinvd" ::: "memory");
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
/* A118: радикальное восстановление после серии неудачных транзактов —
   GCR-сброс шины + re-enable всех подключённых портов + штатный reset порта.
   Вызывается из usb_enumerate перед ПОСЛЕДНЕЙ попыткой GET_DESCRIPTOR. */
int  uhci_hard_recovery(uhci_hc_t *hc, int port);
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

/* ==================== Компактный лог: коды диагностики транзактов ============
   Буфер лога загрузки мал (~30 строк), поэтому однотипные сообщения серии
   схлопываются в "XFER <текст> (xN)". uhci_log_code() вызывается на КАЖДОМ
   завершении транзакта с кодом из списка ниже (0 = успех, не печатается);
   uhci_log_flush() — при переходе к новой фазе enumeration. */
#define ULOG_XFER_TIMEOUT  1   /* TD остался ACTIVE: HC не тронул очередь */
#define ULOG_XFER_NORESP   2   /* received-PID NONE/ERR: устройство молчит */
#define ULOG_XFER_STALL    3   /* device STALL (halted ep) */
#define ULOG_XFER_CRC      4   /* CRC/bitstuff: пакеты искажаются на линии */
#define ULOG_XFER_SHORT    5   /* короткий пакет / DBUFERR */
const char *uhci_xfer_code_text(int code);
void        uhci_log_code(int code);
void        uhci_log_flush(void);

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
