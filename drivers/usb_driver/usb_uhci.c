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
 * usb_uhci.c - UHCI хост-контроллер: PCI probe, reset, frame list,
 * port reset, control/bulk transfer.
 *
 * Один HC (uhci0). QH/TD/Frame List — статические (.bss драйвера,
 * identity map: VA == PA). Control и Bulk работают синхронно через
 * общий Frame List: очередь подключается к слотам только на время
 * транзакции, между вызовами все слоты терминируются по обычному принципу
 */

#include "include/usb_uhci.h"
#include "include/usb_driver.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

//---------------- UHCI DMA-структуры (динамическая аллокация) ----------------
//Все структуры, которые читает/пишет HC (Frame List, QH, TD), должны быть физически
//непрерывными и identity-mapped. Аллоцируются через usb_dma_alloc() -> kernel_usb_dma_alloc() ->
//pmm_alloc_pages()
//dma_alloc_ptr устанавливается ядром в use_drivers.c ПОСЛЕ driver_main(),
//поэтому allocation делается в device_usb_start() (см. usb_driver.c)

uint8_t  *frame_list_ptr   = NULL;
uhci_qh_t *control_qh_ptr  = NULL;
uhci_td_t *control_tds_ptr = NULL;
uint8_t  *setup_static_ptr = NULL;
uhci_qh_t *bulk_qh_ptr     = NULL;
uhci_td_t *bulk_tds_ptr    = NULL;

//Физические адреса (для DMA). На x86 identity-mapped: VA == PA, но
//храним отдельно, чтобы код был корректен и для будущих IOMMU-кейсов
static uint32_t frame_list_phys   = 0;
static uint32_t control_qh_phys   = 0;
static uint32_t control_tds_phys  = 0;
static uint32_t setup_static_phys = 0;
static uint32_t bulk_qh_phys      = 0;
static uint32_t bulk_tds_phys     = 0;

//Флаги "выделено" для cleanup()
static int dma_frame_ok    = 0;
static int dma_ctrl_qh_ok  = 0;
static int dma_ctrl_tds_ok = 0;
static int dma_setup_ok    = 0;
static int dma_bulk_qh_ok  = 0;
static int dma_bulk_tds_ok = 0;

uint8_t uhci_dev_is_ls[128]= {0};

//Глобальный массив всех найденных UHCI-контроллеров
//На ICH6 их три: 0:1d.0 (#1), 0:1d.1 (#2), 0:1d.2 (#3).
//Устройства могут быть на любом из них — нужно сканировать все.
uhci_hc_t uhci_hcs[UHCI_MAX_CONTROLLERS];
int uhci_hc_count = 0;


//Барьеры памяти для DMA. На x86 это инструкции, гарантирующие порядок записей/чтений.
//Для UHCI критично, чтобы контроллер увидел полностью сформированный TD/QH до того,
//как он будет ему передан
static inline void uhci_wmb(void)
{
    __asm__ __volatile__("sfence" ::: "memory");
}
static inline void uhci_mb(void)
{
    __asm__ __volatile__("mfence" ::: "memory");
}
static inline void uhci_rmb(void)
{
    __asm__ __volatile__("lfence" ::: "memory");
}

//Текущие системные тики (10 мс/тик). 0 если API ещё не настроен
static inline uint32_t uhci_get_ticks(void)
{
    return (api_ptr && api_ptr->get_ticks) ? api_ptr->get_ticks() : 0;
}

//UHCI — bus-master, ему нужен физический адрес. Stack/driver .bss
//identity-mapped (VA==PA), но kmalloc-буферы живут по VA != PA
//Делаем walk по текущим page tables через CR3
uint32_t usb_virt_to_phys(uint32_t va)
{
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= 0xFFFFF000u;

    uint32_t *pd = (uint32_t*)cr3;
    uint32_t pde = pd[va >> 22];
    if (!(pde & 1)) return va;                 //Не найден, даем fallback
    uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000u);
    uint32_t pte = pt[(va >> 12) & 0x3FF];
    if (!(pte & 1)) return va;

    return (pte & 0xFFFFF000u) | (va & 0xFFF);
}


//Выделить DMA-структуры через ядро. Идемпотентна — повторный вызов
//без предварительного uhci_free_structures() вернёт 0 (already allocated)
//На любой ошибке частично выделенное освобождается и возвращается отрицательный код
int uhci_alloc_structures(void)
{
    uint32_t phys;
    uint32_t size;

    if (frame_list_ptr != NULL)
    {
        return 0;   /* уже выделено */
    }

    //Frame List: 4096 байт, выровнен на 4K (pmm_alloc_pages даёт 4K-aligned)
    frame_list_ptr = (uint8_t*)usb_dma_alloc(4096, &phys);
    if (!frame_list_ptr || (phys & 0xFFF) != 0)
    {
        uhci_free_structures();
        return -1;
    }
    dma_frame_ok    = 1;
    frame_list_phys = phys;
    memset(frame_list_ptr, 0, 4096);

    //Control QH: 16 байт (физ. адрес — на границе 16)
    control_qh_ptr = (uhci_qh_t*)usb_dma_alloc(sizeof(uhci_qh_t), &phys);
    if (!control_qh_ptr || (phys & 0x0F) != 0)
    {
        uhci_free_structures();
        return -2;
    }
    dma_ctrl_qh_ok   = 1;
    control_qh_phys  = phys;
    memset(control_qh_ptr, 0, sizeof(uhci_qh_t));

    //Control TD pool: MAX_CONTROL_TDS * 16 = 256 байт
    size = MAX_CONTROL_TDS * sizeof(uhci_td_t);
    control_tds_ptr = (uhci_td_t*)usb_dma_alloc(size, &phys);
    if (!control_tds_ptr || (phys & 0x0F) != 0)
    {
        uhci_free_structures();
        return -3;
    }
    dma_ctrl_tds_ok   = 1;
    control_tds_phys  = phys;
    memset(control_tds_ptr, 0, size);

    //Bulk QH: 16 байт
    bulk_qh_ptr = (uhci_qh_t*)usb_dma_alloc(sizeof(uhci_qh_t), &phys);
    if (!bulk_qh_ptr || (phys & 0x0F) != 0)
    {
        uhci_free_structures();
        return -4;
    }
    dma_bulk_qh_ok = 1;
    bulk_qh_phys   = phys;
    memset(bulk_qh_ptr, 0, sizeof(uhci_qh_t));

    //Bulk TD pool: MAX_BULK_TDS * 16 = 1024 байт
    size = MAX_BULK_TDS * sizeof(uhci_td_t);
    bulk_tds_ptr = (uhci_td_t*)usb_dma_alloc(size, &phys);
    if (!bulk_tds_ptr || (phys & 0x0F) != 0)
    {
        uhci_free_structures();
        return -5;
    }
    dma_bulk_tds_ok = 1;
    bulk_tds_phys   = phys;
    memset(bulk_tds_ptr, 0, size);

    //Setup-буфер: 8 байт (для SETUP-пакета control-transfers)
    setup_static_ptr = (uint8_t*)usb_dma_alloc(8, &phys);
    if (!setup_static_ptr || (phys & 0x0F) != 0)
    {
        uhci_free_structures();
        return -6;
    }
    dma_setup_ok       = 1;
    setup_static_phys  = phys;
    memset(setup_static_ptr, 0, 8);

    return 0;
}

//Освободить все DMA-структуры. Безопасна для частично выделенного состояния (флаги dma_*_ok)
//Не вызывать, пока HC ещё жив и ссылается на эти структуры
void uhci_free_structures(void)
{
    if (dma_setup_ok    && setup_static_ptr)
    {
        usb_dma_free(setup_static_ptr, 8);
        dma_setup_ok    = 0;
    }
    if (dma_bulk_tds_ok && bulk_tds_ptr)
    {
        usb_dma_free(bulk_tds_ptr, MAX_BULK_TDS * sizeof(uhci_td_t));
        dma_bulk_tds_ok = 0;
    }
    if (dma_bulk_qh_ok  && bulk_qh_ptr)
    {
        usb_dma_free(bulk_qh_ptr, sizeof(uhci_qh_t));
        dma_bulk_qh_ok  = 0;
    }
    if (dma_ctrl_tds_ok && control_tds_ptr)
    {
        usb_dma_free(control_tds_ptr, MAX_CONTROL_TDS * sizeof(uhci_td_t));
        dma_ctrl_tds_ok = 0;
    }
    if (dma_ctrl_qh_ok  && control_qh_ptr)
    {
        usb_dma_free(control_qh_ptr, sizeof(uhci_qh_t));
        dma_ctrl_qh_ok  = 0;
    }
    if (dma_frame_ok    && frame_list_ptr)
    {
        usb_dma_free(frame_list_ptr, 4096);
        dma_frame_ok    = 0;
    }
    frame_list_ptr   = NULL;
    control_qh_ptr   = NULL;
    control_tds_ptr  = NULL;
    setup_static_ptr = NULL;
    bulk_qh_ptr      = NULL;
    bulk_tds_ptr     = NULL;
    frame_list_phys   = 0;
    control_qh_phys   = 0;
    control_tds_phys  = 0;
    setup_static_phys = 0;
    bulk_qh_phys      = 0;
    bulk_tds_phys     = 0;
}



//Проверка состояния HC во время ожидания TD. Возвращает 0 — HC жив и работает; <0 — фатальная проблема
int uhci_hc_healthy(uhci_hc_t *hc)
{
    uint16_t sts = inw(hc->io_base + UHCI_USBSTS);
    if (sts & USBSTS_HSE)
    {
        outw(hc->io_base + UHCI_USBSTS, USBSTS_HSE);   //W1C
        return -1;   //Host System Error (PCI abort и т.п.)
    }
    if (sts & USBSTS_HCH)
    {
        return -2;   //HC Halted — контроллер остановлен
    }
    return 0;
}


//---------------- PCI ----------------

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = (1u << 31)
                    | ((uint32_t)bus  << 16)
                    | ((uint32_t)dev  << 11)
                    | ((uint32_t)func <<  8)
                    | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    uint32_t addr = (1u << 31)
                    | ((uint32_t)bus  << 16)
                    | ((uint32_t)dev  << 11)
                    | ((uint32_t)func <<  8)
                    | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}


//---------------- Probe ----------------

//Найти ВСЕ UHCI-контроллеры в PCI. Заполняет uhci_hcs[], возвращает количество
int uhci_probe_all(void)
{
    uhci_hc_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t dev = 0; dev < 32; dev++)
        {
            for (uint8_t func = 0; func < 8; func++)
            {
                if (uhci_hc_count >= UHCI_MAX_CONTROLLERS) return uhci_hc_count;

                uint32_t id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t class = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t base_cls = (class >> 24) & 0xFF;
                uint8_t sub_cls  = (class >> 16) & 0xFF;
                uint8_t prog_if  = (class >>  8) & 0xFF;

                if (base_cls != PCI_CLASS_SERIAL_BUS ||
                        sub_cls  != PCI_SUBCLASS_USB ||
                        prog_if  != PCI_PROGIF_UHCI) continue;

                uint32_t bar4 = pci_read32((uint8_t)bus, dev, func, 0x20);
                if (!(bar4 & 0x01)) continue;
                uint16_t io_base = bar4 & 0xFFFC;
                if (io_base == 0) continue;

                uint32_t cmd = pci_read32((uint8_t)bus, dev, func, 0x04);
                cmd |= 0x05;
                pci_write32((uint8_t)bus, dev, func, 0x04, cmd);

                uint32_t vendor_dev = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((vendor_dev & 0xFFFF) == 0x8086)
                {
                    pci_write32((uint8_t)bus, dev, func, 0xC0, 0x2000);
                }

                uhci_hc_t *hc = &uhci_hcs[uhci_hc_count];
                memset(hc, 0, sizeof(*hc));
                hc->io_base  = io_base;
                hc->pci_bus  = (uint8_t)bus;
                hc->pci_dev  = dev;
                hc->pci_func = func;
                uhci_hc_count++;
            }
        }
    }
    return uhci_hc_count;
}

//---------------- Init ----------------

int uhci_wait_hcreset(uhci_hc_t *hc)
{
    for (uint32_t i = 0; i < 100000; i++)
    {
        if (!(inw(hc->io_base + UHCI_USBCMD) & USBCMD_HCRESET)) return 0;
    }
    return -1;
}

int uhci_init(uhci_hc_t *hc)
{
    if (!hc) return -1;

    //Legacy mode BIOS: если CF=0, BIOS оставил HC в режиме, где firmware делит управление с ОС
    //Принудительно переводим в полный OS-контроль
    //Global reset: сбрасывает HC и все порты в исходное состояние.
    //На ICH3 после BIOS порты могут быть в SUSPEND/SDE/legacy — global reset
    //это очищает. Делаем ДО HCRESET, чтобы HC точно был в чистом состоянии.
    outw(hc->io_base + UHCI_USBCMD, USBCMD_GRESET | USBCMD_CF);
    for (volatile int i = 0; i < 30000; i++) inw(hc->io_base + UHCI_USBCMD);
    outw(hc->io_base + UHCI_USBCMD, USBCMD_CF);

    //Host Controller Reset
    outw(hc->io_base + UHCI_USBCMD, USBCMD_HCRESET);
    if (uhci_wait_hcreset(hc) != 0) return -2;

    //Отключить все прерывания — работаем polling'ом
    outw(hc->io_base + UHCI_USBINTR, 0);

    //Очистить статус (все W1C биты: USBINT, ERROR, RESET, RESUME, HSE)
    outw(hc->io_base + UHCI_USBSTS, 0x007F);

    //Ложный overcurrent на старых ICH (PIIX4/ICH3): бит 7 PORTSC
    //может быть установлен постоянно, аппаратно отключая порт.
    //Игнорируем его — пишем 1 для очистки change-бита (если это R/WC)
    //и принудительно включаем порт позже через reset.
    for (int p = 0; p < 2; p++)
    {
        uint16_t preg = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t ps = inw(hc->io_base + preg);
        if (ps & 0x0080)                  //OverCurrent Indicator / Change
        {
            outw(hc->io_base + preg, ps);  //write-1-to-clear все change-биты
        }
    }

    //Frame List — все слоты terminate (Vf=1, нет QH)
    uint32_t *fl = (uint32_t*)frame_list_ptr;
    for (int i = 0; i < 1024; i++) fl[i] = TD_LINK_TERM;
    //Физический адрес = виртуальный (identity map нижних 16 МБ)
    outl(hc->io_base + UHCI_FLBASEADD, frame_list_phys);

    uhci_mb();   //гарантируем, что контроллер увидит новый Frame List

    outw(hc->io_base + UHCI_FRNUM, 0);
    outw(hc->io_base + UHCI_SOFMOD, 64);   //64 байта на кадр

    //Запуск HC: CF=1 (полный OS-контроль, без legacy режима) + RS=1.
    //CF обязателен. Без него на ICH3/ICH4 HC остаётся в legacy BIOS
    //режиме, порты не сканируются, CCS не выставляется. QEMU этого
    //не проверяет (там HC всегда "configured"), а реальное железо — да
    uint16_t cmd = inw(hc->io_base + UHCI_USBCMD);
    outw(hc->io_base + UHCI_USBCMD, cmd | USBCMD_CF | USBCMD_RS);

    //Дождаться, что HC снял HCH и реально запустился
    for (uint32_t i = 0; i < 100000; i++)
    {
        uint16_t sts = inw(hc->io_base + UHCI_USBSTS);
        if (!(sts & USBSTS_HCH))
        {
            //Дополнительная проверка: CF=1 и RS=1 реально установились
            uint16_t cmd_now = inw(hc->io_base + UHCI_USBCMD);
            if ((cmd_now & (USBCMD_CF | USBCMD_RS)) != (USBCMD_CF | USBCMD_RS))
            {
                return -4;   //HC не принял CF/RS — платформа нестандартна
            }
            return 0;
        }
    }
    return -3;
}

//---------------- Ports ----------------

uint16_t portsc_reg(int port)
{
    return (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
}

uint16_t uhci_port_status(uhci_hc_t *hc, int port)
{
    return inw(hc->io_base + portsc_reg(port));
}

int uhci_port_connected(uhci_hc_t *hc, int port)
{
    return (uhci_port_status(hc, port) & PORTSC_CCS) ? 1 : 0;
}

int uhci_reset_port(uhci_hc_t *hc, int port)
{
    uint16_t reg = portsc_reg(port);

    uint16_t pre = inw(hc->io_base + reg);
    if (pre & (PORTSC_SUSPEND | PORTSC_SDE))
    {
        outw(hc->io_base + reg, pre & ~(PORTSC_SUSPEND | PORTSC_SDE));
        for (volatile int i = 0; i < 10000; i++) inw(hc->io_base + reg);
    }

    uint16_t s = inw(hc->io_base + reg);
    if (s & PORTSC_DS) return -3;        //debug port — не трогаем

    //Если CCS=0, но установлен overcurrent (бит 7) — это, скорее всего,
    //ложное срабатывание на старом ICH. Пробуем сбросить порт всё равно:
    //после reset overcurrent-состояние может уйти
    if (!(s & PORTSC_CCS))
    {
        if (s & 0x0080)
        {
            //Очистить change-бит overcurrent, записав 1
            outw(hc->io_base + reg, s);
            for (volatile int i = 0; i < 10000; i++) inw(hc->io_base + reg);
            s = inw(hc->io_base + reg);
            //Если CCS всё ещё 0 — устройство действительно не подключено
            //(или overcurrent реальный). Продолжаем reset на всякий случай:
            //reset может выбить залипшее overcurrent-состояние
        }
        else
        {
            return -1; //нет устройства и нет overcurrent
        }
    }

    //Assert: PR=1, PED=1
    outw(hc->io_base + reg, PORTSC_PR | PORTSC_PED);

    //Держим PR=1 ≥ 50 мс (USB 2.0 §7.1.7.3). Используем тики ядра (10 мс), чтобы
    //время было одинаковым на любой частоте CPU. Safety-net по итерациям — на случай если тики
    //почему-то не идут (например, PIT не запущен, или мы до multithreading)
    uint32_t t_reset = uhci_get_ticks();
    uint32_t safety = 10000000;
    while (((uhci_get_ticks() - t_reset) < 5) && (safety-- > 0))
    {
        inw(hc->io_base + reg);
    }

    //Deassert: читаем свежий PORTSC, снимаем PR, PED форсируем =1
    uint16_t cur = inw(hc->io_base + reg);
    outw(hc->io_base + reg, (cur & ~PORTSC_PR) | PORTSC_PED);

    //Дать порту стабилизироваться ≥ 10 мс (reset recovery time, USB 2.0 §7.1.7.5)
    uint32_t t_settle = uhci_get_ticks();
    safety = 5000000;
    while (((uhci_get_ticks() - t_settle) < 1) && (safety-- > 0))
    {
        inw(hc->io_base + reg);
    }

    uint16_t fin = inw(hc->io_base + reg);
    outw(hc->io_base + reg, fin | PORTSC_CSC | PORTSC_PEDC | PORTSC_PED);

    //Финальная проверка
    uint32_t t0 = uhci_get_ticks();
    while ((uhci_get_ticks() - t0) < 50)    //50 тиков * 10 мс = 500 мс запас
    {
        s = inw(hc->io_base + reg);
        if (s & PORTSC_PED) return 0;
        if (s & PORTSC_DS)  return -3;      //debug port
    }
    return -2;
}

//---------------- Control transfer ----------------

int uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr,
                          const uint8_t *setup_pkt,
                          void *data_buf, uint16_t data_len,
                          int data_in, uint16_t max_pkt)
{

    if (!hc || !setup_pkt) return -1;
    if (max_pkt == 0 || max_pkt > 64) max_pkt = 8;

    //Low-speed устройство (клавиатура, мышь, тачпад): нужен TD_CTRL_LS в статусе всех TD,
    //иначе HC отправит FS-пакет и LS-устройство его не примет
    uint32_t ls_bit = (dev_addr < 128 && uhci_dev_is_ls[dev_addr])
                      ? TD_CTRL_LS : 0;

    //Сколько TD понадобится под data stage (по одному на пакет)
    uint32_t num_data = 0;
    int has_data = (data_len > 0 && data_buf != NULL);
    if (has_data)
    {
        num_data = (data_len + max_pkt - 1) / max_pkt;
        if (num_data > MAX_CONTROL_TDS - 2)
        {
            num_data = MAX_CONTROL_TDS - 2;   //обрезаем, если не влезает
        }
    }
    uint32_t total_tds = 1 + num_data + 1;    //setup + data + status

    //Setup packet копируем в статику (identity-map)
    for (int i = 0; i < 8; i++) setup_static_ptr[i] = setup_pkt[i];

    uhci_td_t *tds = control_tds_ptr;
    uint8_t   *buf = (uint8_t *)data_buf;

    //TD[0]: SETUP, DATA0, 8 байт
    tds[0].link   = (control_tds_phys + sizeof(uhci_td_t)) & ~0x0Fu;
    tds[0].status = TD_CTRL_ACTIVE | TD_CTRL_MAXERR_3 | ls_bit;
    tds[0].token  = ((uint32_t)PID_SETUP << TD_TOKEN_PID_SHIFT)
                    | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                    | (0u << TD_TOKEN_EP_SHIFT)
                    | 0u                                              //DATA0
                    | ((uint32_t)(8 - 1) << TD_TOKEN_LEN_SHIFT);
    tds[0].buffer = setup_static_phys;

    //TD[1..num_data]: Data stage, чанки по max_pkt
    //Toggle в data stage начинается с DATA1 и чередуется:
    //i=0 -> DATA1, i=1 -> DATA0, i=2 -> DATA1, ...
    for (uint32_t i = 0; i < num_data; i++)
    {
        uint32_t idx   = 1 + i;
        uint32_t off   = i * max_pkt;
        uint32_t chunk = (data_len - off > max_pkt) ? max_pkt
                         : (data_len - off);
        uint32_t toggle = ((i & 1) == 0) ? TD_TOKEN_TOGGLE : 0u;
        uint32_t next_idx = (i + 1 < num_data) ? (idx + 1) : (1 + num_data);
        uint32_t next   = (control_tds_phys + next_idx * sizeof(uhci_td_t)) & ~0x0Fu;

        tds[idx].link   = next;
        //tds[idx].status = TD_CTRL_ACTIVE | TD_CTRL_MAXERR_3;
        uint32_t spd = data_in ? TD_CTRL_SPD : 0;
        tds[idx].status = TD_CTRL_ACTIVE | TD_CTRL_MAXERR_3 | spd | ls_bit;
        tds[idx].token  = ((uint32_t)(data_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT)
                          | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                          | (0u << TD_TOKEN_EP_SHIFT)
                          | toggle
                          | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        tds[idx].buffer = usb_virt_to_phys((uint32_t)(buf + off));
    }

    //Status TD: направление противоположно data, всегда DATA1, 0 байт
    uint32_t sidx       = 1 + num_data;
    uint32_t status_pid = has_data ? (data_in ? PID_OUT : PID_IN) : PID_IN;

    tds[sidx].link   = TD_LINK_TERM;
    tds[sidx].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_MAXERR_3 | ls_bit;
    tds[sidx].token  = (status_pid << TD_TOKEN_PID_SHIFT)
                       | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                       | (0u << TD_TOKEN_EP_SHIFT)
                       | TD_TOKEN_TOGGLE
                       | ((uint32_t)0x7FF << TD_TOKEN_LEN_SHIFT);
    tds[sidx].buffer = setup_static_phys;

    //--- QH ---
    control_qh_ptr->head    = control_tds_phys & ~0x0Fu;
    control_qh_ptr->element = TD_LINK_TERM;

    //Гарантируем, что контроллер увидит полностью сформированные TD до того,
    //как QH будет подключён к Frame List
    uhci_wmb();

    uint32_t qh_phys_val = control_qh_phys;
    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = (qh_phys_val & ~0x0Fu) | TD_LINK_Q;

    //Ожидание
    uint32_t timeout = 50000000;   //safety-net по итерациям
    uint32_t nak_retries = 0;
    uint32_t start_tick = uhci_get_ticks();
    //200 тиков * 10 мс = 2 секунды. Гранулярность 10 мс, точность <0.5 %
    const uint32_t CTRL_TIMEOUT_TICKS = UHCI_TICKS_PER_SEC * UHCI_CTRL_TIMEOUT_SEC;

    int result = -3;
    while (timeout--)
    {
        if ((timeout & 0x3FF) == 0)
        {
            if (uhci_hc_healthy(hc) != 0)
            {
                result = -4;
                break;
            }
        }
        //(current - start) корректно через переполнение uint32_t:
        //пока timeout < 2^31 тиков (248 суток), всё работает
        if ((timeout & 0xFF) == 0 &&
                (uhci_get_ticks() - start_tick) > CTRL_TIMEOUT_TICKS)
        {
            result = -5;
            break;
        }

        uint32_t st = tds[sidx].status;
        if (!(st & TD_CTRL_ACTIVE))
        {
            //Только реальные ошибки — NAK сюда не входит, он штатный
            uint32_t hard_bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR |
                                TD_CTRL_BABBLE  | TD_CTRL_CRCTIMEO;

            //Проверить все TD на ошибки и NAK
            int hard_err  = 0;
            int nak_seen  = 0;
            for (uint32_t i = 0; i < total_tds; i++)
            {
                uint32_t kst = tds[i].status;
                if (kst & hard_bad)
                {
                    hard_err = 1;
                    break;
                }
                if (kst & TD_CTRL_NAK) nak_seen = 1;
            }
            if (hard_err)
            {
                result = -2;
                break;
            }

            if (nak_seen)
            {
                //Retry: toggle-механизм USB защитит от дублирования данных
                //(device discarded+ACK на повторный toggle)
                //Safety-net от бесконечного NAK — nak_retries и TSC
                if (++nak_retries > 1000)
                {
                    result = -5;
                    break;
                }
                for (uint32_t i = 0; i < total_tds; i++)
                {
                    tds[i].status = TD_CTRL_ACTIVE | TD_CTRL_MAXERR_3 | ls_bit;
                    if (i == sidx) tds[i].status |= TD_CTRL_IOC;
                    //SPD — только на data-stage IN-TD (setup и status его не должны иметь по спеке)
                    if (data_in && i >= 1 && i <= num_data)
                    {
                        tds[i].status |= TD_CTRL_SPD;
                    }
                }
                uhci_wmb();
                continue;
            }

            //Все TD прошли успешно
            result = (int)data_len;
            break;
        }
    }

    //Снять ACTIVE со всех TD, чтобы они были чистыми к следующему вызову
    for (uint32_t i = 0; i < total_tds; i++)
    {
        tds[i].status &= ~TD_CTRL_ACTIVE;
    }

    //После завершения TD гарантируем, что CPU увидит данные, записанные контроллером
    //(актуально для IN-передач)
    uhci_rmb();

    //Диагностика неудачи control-transfer: покажем код, финальный статус TD
    //и PORTSC — по ним видно, STALL это, timeout, или порт выключился.
    if (result < 0 && api_ptr && api_ptr->load_screen_add_string)
    {
        api_ptr->load_screen_add_string("ctrl xfer: r=%x st=%x ls=%d addr=%d",
                                        (uint32_t)result, tds[sidx].status, (int)(ls_bit ? 1 : 0), dev_addr);
    }

    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = TD_LINK_TERM;

    (void)total_tds;
    return result;
}

int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr,
                       uint16_t ep_max_pkt, uint8_t *toggle,
                       void *buf, uint32_t len, int dir_in)
{

    if (!hc || !buf || len == 0 || !toggle) return -1;
    if (ep_max_pkt == 0 || ep_max_pkt > 1024) ep_max_pkt = 64;

    uint32_t num_tds = (len + ep_max_pkt - 1) / ep_max_pkt;
    if (num_tds > MAX_BULK_TDS)
    {
        num_tds = MAX_BULK_TDS;
        len = num_tds * ep_max_pkt;    //режем запрос до влезающего
    }

    uint8_t ep_num = ep_addr & 0x0F;
    uint8_t cur_toggle = *toggle ? 1 : 0;
    uint8_t *b = (uint8_t *)buf;
    uhci_td_t *tds = bulk_tds_ptr;

    for (uint32_t i = 0; i < num_tds; i++)
    {
        uint32_t off   = i * ep_max_pkt;
        uint32_t chunk = (len - off > ep_max_pkt) ? ep_max_pkt : (len - off);
        uint32_t toggle_bit = cur_toggle ? TD_TOKEN_TOGGLE : 0u;
        uint32_t next = (i + 1 < num_tds)
                        ? ((bulk_tds_phys + (i + 1) * sizeof(uhci_td_t)) & ~0x0Fu)
                        : TD_LINK_TERM;

        tds[i].link   = next;
        //tds[i].status = TD_CTRL_ACTIVE | (i == num_tds - 1 ? TD_CTRL_IOC : 0u) | TD_CTRL_MAXERR_3;
        uint32_t spd = dir_in ? TD_CTRL_SPD : 0;
        tds[i].status = TD_CTRL_ACTIVE | TD_CTRL_MAXERR_3 | spd | (i == num_tds - 1 ? TD_CTRL_IOC : 0u);
        tds[i].token  = ((uint32_t)(dir_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT)
                        | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT)
                        | ((uint32_t)ep_num << TD_TOKEN_EP_SHIFT)
                        | toggle_bit
                        | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        tds[i].buffer = usb_virt_to_phys((uint32_t)(b + off));

        cur_toggle ^= 1;
    }

    bulk_qh_ptr->head    = bulk_tds_phys & ~0x0Fu;
    bulk_qh_ptr->element = TD_LINK_TERM;

    uhci_wmb();

    uint32_t qh_phys_val = bulk_qh_phys;
    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = (qh_phys_val & ~0x0Fu) | TD_LINK_Q;

    uint32_t timeout = 100000000;   //safety-net по итерациям
    uint32_t nak_retries = 0;
    uint32_t start_tick = uhci_get_ticks();
    //500 тиков * 10 мс = 5 секунд
    const uint32_t BULK_TIMEOUT_TICKS = UHCI_TICKS_PER_SEC * UHCI_BULK_TIMEOUT_SEC;

    int result = -3;
    while (timeout--)
    {
        if ((timeout & 0x3FF) == 0)
        {
            if (uhci_hc_healthy(hc) != 0)
            {
                result = -4;
                break;
            }
        }
        if ((timeout & 0xFF) == 0 &&
                (uhci_get_ticks() - start_tick) > BULK_TIMEOUT_TICKS)
        {
            result = -5;
            break;
        }

        uint32_t st = tds[num_tds - 1].status;
        if (!(st & TD_CTRL_ACTIVE))
        {
            uint32_t hard_bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR |
                                TD_CTRL_BABBLE  | TD_CTRL_CRCTIMEO;

            int hard_err  = 0;
            int nak_seen  = 0;
            for (uint32_t k = 0; k < num_tds; k++)
            {
                uint32_t kst = tds[k].status;
                if (kst & hard_bad)
                {
                    hard_err = 1;
                    break;
                }
                if (kst & TD_CTRL_NAK) nak_seen = 1;
            }
            if (hard_err)
            {
                result = -2;
                break;
            }

            if (nak_seen)
            {
                //Retry: toggle-механизм USB гарантирует, что уже ACK'нутые пакеты не будут приняты
                //повторно (discard+ACK)
                if (++nak_retries > 1000)
                {
                    result = -5;
                    break;
                }
                for (uint32_t i = 0; i < num_tds; i++)
                {
                    tds[i].status = TD_CTRL_ACTIVE | TD_CTRL_MAXERR_3;
                    if (i == num_tds - 1) tds[i].status |= TD_CTRL_IOC;
                    if (dir_in) tds[i].status |= TD_CTRL_SPD;
                }
                uhci_wmb();
                continue;
            }

            //Все TD успешно завершены
            *toggle = cur_toggle;
            result = (int)len;
            break;
        }
    }

    //Снять ACTIVE со всех TD
    for (uint32_t k = 0; k < num_tds; k++)
    {
        tds[k].status &= ~TD_CTRL_ACTIVE;
    }

    uhci_rmb();   //для IN-передач: данные точно видны CPU

    for (int i = 0; i < 1024; i++)
        *(uint32_t*)&frame_list_ptr[i*4] = TD_LINK_TERM;

    return result;
}



int uhci_check_alignment(void)
{
    //Если структуры ещё не выделены — это ошибка порядка вызова
    if (!frame_list_ptr || !control_qh_ptr || !control_tds_ptr ||
            !bulk_qh_ptr || !bulk_tds_ptr || !setup_static_ptr)
    {
        return -1;
    }

    int bad = 0;
    if ((frame_list_phys   & 0xFFF) != 0) bad |= 1;
    if ((control_qh_phys   & 0x0F)  != 0) bad |= 2;
    if ((control_tds_phys  & 0x0F)  != 0) bad |= 4;
    if ((bulk_qh_phys      & 0x0F)  != 0) bad |= 8;
    if ((bulk_tds_phys     & 0x0F)  != 0) bad |= 16;
    if ((setup_static_phys & 0x0F)  != 0) bad |= 32;

    return bad;
}

