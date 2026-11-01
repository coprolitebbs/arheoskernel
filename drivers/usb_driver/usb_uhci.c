#include "include/usb_uhci.h"
#include "include/usb_driver.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

static uint8_t raw_frame_buffer[4096 * 2] __attribute__((aligned(16)));
uint32_t *frame_list_ptr = NULL;
uhci_qh_t control_qh __attribute__((aligned(16)));
uhci_qh_t *control_qh_ptr;
uhci_td_t control_tds[MAX_CONTROL_TDS] __attribute__((aligned(16)));
uhci_td_t *control_tds_ptr;
uint8_t setup_static[32] __attribute__((aligned(16)));
uint8_t *setup_static_ptr;

uhci_qh_t bulk_qh __attribute__((aligned(16)));
uhci_qh_t *bulk_qh_ptr;
uhci_td_t bulk_tds[MAX_BULK_TDS] __attribute__((aligned(16)));
uhci_td_t *bulk_tds_ptr;

uhci_queue_t g_control_queue;
uhci_queue_t g_bulk_queue;

uint32_t usb_virt_to_phys(uint32_t va)
{
    // ЖЕСТКИЙ АППАРАТНЫЙ ФИКС: Убираем ручной обход CR3, который возвращал мусор на реальном ПК!
    // Поскольку код драйвера выполняется в Identity-Mapped (плоской) нижней памяти ядра,
    // виртуальный Си-адрес переменной гарантированно равен её физическому адресу в ОЗУ.
    // Возвращаем flat-адрес напрямую процессору и Bus-Master контроллеру UHCI!
    return va;
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

int uhci_probe(uhci_hc_t *hc)
{
    return 0;    // Больше не нужен, probe делает usb_driver.c
}

int uhci_wait_hcreset(uhci_hc_t *hc)
{
    for (uint32_t i = 0; i < 100000; i++)
    {
        if (!(inw(hc->io_base + UHCI_USBCMD) & USBCMD_HCRESET)) return 0;
    }
    return -1;
}

void uhci_init_queues(void)
{
    memset(&g_control_queue, 0, sizeof(uhci_queue_t));
    memset(&g_bulk_queue, 0, sizeof(uhci_queue_t));

    uint32_t dummy_ctrl_phys = usb_virt_to_phys((uint32_t)&g_control_queue.dummy_td);
    g_control_queue.qh.element = dummy_ctrl_phys & ~0x0F;
    g_control_queue.dummy_td.link = TD_LINK_TERM;
    g_control_queue.dummy_td.status = 0;
    g_control_queue.tail_td = &g_control_queue.dummy_td;

    uint32_t dummy_bulk_phys = usb_virt_to_phys((uint32_t)&g_bulk_queue.dummy_td);
    g_bulk_queue.qh.element = dummy_bulk_phys & ~0x0F;
    g_bulk_queue.dummy_td.link = TD_LINK_TERM;
    g_bulk_queue.dummy_td.status = 0;
    g_bulk_queue.tail_td = &g_bulk_queue.dummy_td;

    memset(control_tds, 0, sizeof(control_tds));
    memset(bulk_tds, 0, sizeof(bulk_tds));
}

int uhci_init(uhci_hc_t *hc)
{
    if (!hc) return -1;

    // 1) Гарантируем маску 0x07 на шине PCI для удержания Bus Master и Memory Space
    uint32_t pci_cmd = pci_read32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x04);
    pci_write32(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x04, (pci_cmd & ~0x7u) | 0x07u);

    // Мягко останавливаем маршевый движок BIOS (RS = 0)
    uint16_t cmd = inw(hc->io_base + UHCI_USBCMD);
    outw(hc->io_base + UHCI_USBCMD, cmd & ~USBCMD_RS);
    for (uint32_t i = 0; i < 100000; i++) {
        if (inw(hc->io_base + UHCI_USBSTS) & USBSTS_HCH) break;
    }

    // Инициализируем наши новые скелетные асинхронные очереди (Control & Bulk)
    uhci_init_queues();

    uint32_t control_qh_phys = usb_virt_to_phys((uint32_t)&g_control_queue.qh);
    uint32_t bulk_qh_phys    = usb_virt_to_phys((uint32_t)&g_bulk_queue.qh);
    if (!control_qh_phys || !bulk_qh_phys) return -4;

    // Связываем QH в плоскую иерархию: Control QH -> Bulk QH -> TERM
    g_control_queue.qh.head = (bulk_qh_phys & ~0x0Fu) | TD_LINK_Q;
    g_bulk_queue.qh.head    = TD_LINK_TERM;

        // ==================== РАДИКАЛЬНЫЙ РУЧНОЙ ФИКС ВЫРАВНИВАНИЯ FLBASEADD ====================
    // Вычисляем плоский flat-адрес, ГАРАНТИРОВАННО кратный границе 4096 байт (4 КБ)
    uint32_t raw_addr = (uint32_t)raw_frame_buffer;
    uint32_t aligned_addr = (raw_addr + 4095u) & ~4095u; // Ручной сдвиг на начало чистой страницы

    frame_list_ptr = (uint32_t *)aligned_addr;
    uint32_t fl_phys = usb_virt_to_phys(aligned_addr);

    if (!fl_phys || (fl_phys & 0xFFFu)) {
        return -5; // Аварийный выход, если физический адрес имеет смещение
    }

    // Заполняем выровненную таблицу кадров ссылками на нашу Control-очередь g_control_queue
    uint32_t fl_link_val = (control_qh_phys & ~0x0Fu) | TD_LINK_Q;
    for (int i = 0; i < 1024; i++) {
        frame_list_ptr[i] = fl_link_val;
    }

    // ТЕПЕРЬ КРЕМНИЙ INTEL ICH6 НА PANASONIC CF-18 БЕЗУПРЕЧНО ПРИМЕТ НАШ АДРЕС!
    outl(hc->io_base + UHCI_FLBASEADD, fl_phys);
    outw(hc->io_base + UHCI_FRNUM, 0);
    outw(hc->io_base + UHCI_SOFMOD, 64);

    // Подтверждаем включение портов, убирая дребезг статусов
    for (int port = 0; port < 2; port++)
    {
        uint16_t reg = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t ps  = inw(hc->io_base + reg);
        uint16_t warm_mask = (ps & ~(PORTSC_PR | PORTSC_SUSPEND | 0x000Au)) | PORTSC_PED | PORTSC_CSC | PORTSC_PEDC;
        outw(hc->io_base + reg, warm_mask);
        inw(hc->io_base + reg);
    }

    outw(hc->io_base + UHCI_USBSTS, 0x003F);
    outw(hc->io_base + UHCI_USBINTR, 0);

    usb_dma_sync_before();

    // Запускаем наш движок заново (RS = 1, CF = 1)
    outw(hc->io_base + UHCI_USBCMD, USBCMD_RS | USBCMD_CF);
    inw(hc->io_base + UHCI_USBCMD); // PCI Flush

    for (uint32_t i = 0; i < 100000; i++)
    {
        if (!(inw(hc->io_base + UHCI_USBSTS) & USBSTS_HCH)) return 0; // УСПЕХ!
    }
    return -3;
}




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
    if (!hc) return -1;
    uint16_t reg = portsc_reg(port);

    // Считываем теплое рабочее состояние, оставленное нам BIOS
    uint16_t s_fin = inw(hc->io_base + reg);

    // Принудительно подтверждаем чипсету Intel ICH6, что мы перехватили управление:
    // явно оставляем PED=1 (Port Enabled), но выжигаем change-флаги CSC/PEDC (W1C маска)
    uint16_t warm_mask = (s_fin & ~(PORTSC_PR | PORTSC_SUSPEND | 0x000Au)) | PORTSC_PED | PORTSC_CSC | PORTSC_PEDC;
    outw(hc->io_base + reg, warm_mask);
    inw(hc->io_base + reg); // PCI Flush

    // Короткая пауза для стабилизации логики
    for (volatile int i = 0; i < 10000; i++) inw(hc->io_base + reg);

    s_fin = inw(hc->io_base + reg);
    int ped_ok = (s_fin & PORTSC_PED) ? 1 : 0;

    api_ptr->load_screen_add_string("WARM_TAKE_RST[%d] ped=%d fin=%x", port, ped_ok, s_fin);

    if (!ped_ok) return -2;

    // Считываем скорость: 0 = Full-Speed (LSDA = 0)
    hc->current_device_low_speed = (s_fin & PORTSC_LSDA) ? 1 : 0;
    return 0;
}






int uhci_control_transfer(uhci_hc_t *hc, uint8_t dev_addr, const uint8_t *setup_pkt, void *data_buf, uint16_t data_len, int data_in, uint16_t max_pkt)
{
    if (!hc || !setup_pkt) return -1;
    if (max_pkt == 0 || max_pkt > 64) max_pkt = 8;

    uint32_t num_data = 0;
    int has_data = (data_len > 0 && data_buf != NULL);
    if (has_data)
    {
        num_data = (data_len + max_pkt - 1) / max_pkt;
        if (num_data > MAX_CONTROL_TDS - 2) num_data = MAX_CONTROL_TDS - 2;
    }

    // Копируем setup-пакет строго в выровненный flat-буфер
    for (int i = 0; i < 8; i++) setup_static[i] = setup_pkt[i];
    uint32_t setup_phys = usb_virt_to_phys((uint32_t)setup_static);

    // ВЫРАВНИВАНИЕ АДРЕСОВ: Явно извлекаем физические адреса элементов массива по строгим индексам!
    uint32_t td0_phys = usb_virt_to_phys((uint32_t)&control_tds[0]);
    uint32_t td1_phys = usb_virt_to_phys((uint32_t)&control_tds[1]);
    uint32_t qh_phys  = usb_virt_to_phys((uint32_t)&g_control_queue.qh);
    uint32_t dummy_phys = usb_virt_to_phys((uint32_t)&g_control_queue.dummy_td);

    // --- TD0: SETUP Stage ---
    // ЖЕСТКИЙ ФИКС: Используем СТРОГО явный индекс control_tds[0], ликвидируя баг двойных релокаций!
    // Взводим Vertical First (0x04) в поле .link для атомарности фаз чипсета Intel ICH6.
    control_tds[0].link   = (td1_phys & ~0x0Fu) | 0x04u;
    control_tds[0].status = 0x00800000u; // Чистый active без BIOS-мусора C_ERR
    if (hc->current_device_low_speed) control_tds[0].status |= TD_CTRL_LS;
    control_tds[0].token  = (PID_SETUP << TD_TOKEN_PID_SHIFT) | (dev_addr << TD_TOKEN_ADDR_SHIFT) | (7 << TD_TOKEN_LEN_SHIFT);
    control_tds[0].buffer = setup_phys;

    // --- TD[1..num_data]: DATA Stage ---
    for (uint32_t i = 0; i < num_data; i++)
    {
        uint32_t idx = 1 + i;
        uint32_t off = i * max_pkt;
        uint32_t chunk = (data_len - off > max_pkt) ? max_pkt : (data_len - off);
        uint32_t toggle = ((i & 1) == 0) ? 0x00080000u : 0x00000000u;

        uint32_t next_idx = (i + 1 < num_data) ? (idx + 1) : (1 + num_data);
        uint32_t next_td_phys = usb_virt_to_phys((uint32_t)&control_tds[next_idx]);

        // Для всех DATA-пакетов продолжаем удерживать Vertical First (0x04)
        control_tds[idx].link = (next_td_phys & ~0x0Fu) | 0x04u;
        control_tds[idx].status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR_MASK;
        if (hc->current_device_low_speed) control_tds[idx].status |= TD_CTRL_LS;
        control_tds[idx].token = ((data_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT) | (dev_addr << TD_TOKEN_ADDR_SHIFT) | toggle | ((chunk - 1) << TD_TOKEN_LEN_SHIFT);

        uint32_t pa = usb_virt_to_phys((uint32_t)((uint8_t *)data_buf + off));
        if (pa == 0) return -1;
        control_tds[idx].buffer = pa;
    }

    // --- TD[sidx]: STATUS Stage ---
    uint32_t sidx = 1 + num_data;
    uint32_t status_pid = has_data ? (data_in ? PID_OUT : PID_IN) : PID_IN;

    control_tds[sidx].link   = TD_LINK_TERM; // Намертво терминируем по спецификации Linux
    control_tds[sidx].status = TD_CTRL_ACTIVE | TD_CTRL_IOC | TD_CTRL_C_ERR_MASK;
    if (hc->current_device_low_speed) control_tds[sidx].status |= TD_CTRL_LS;

    uint32_t status_toggle = 0x00080000u;
    control_tds[sidx].token = (status_pid << TD_TOKEN_PID_SHIFT) | (dev_addr << TD_TOKEN_ADDR_SHIFT) | status_toggle | (0x7FF << TD_TOKEN_LEN_SHIFT);
    control_tds[sidx].buffer = setup_phys;

    // ЖЕСТКИЙ ФИКС: Перелинковываем линк Setup-пакета строго по индексу control_tds[0]!
    control_tds[0].link = (td1_phys & ~0x0Fu) | 0x04u;

    // Вычисляем точный физический адрес поля .status для финального дескриптора Status Stage
    uint32_t status_td_status_phys = usb_virt_to_phys((uint32_t)&control_tds[sidx].status);

    api_ptr->load_screen_add_string("UHCI DMA Map: QH=%x TD0=%x TD_stat_phys=%x", qh_phys, td0_phys, status_td_status_phys);

    // Принудительно выталкиваем дескрипторы из кэша Write-Back процессора в ОЗУ перед стартом DMA
    __asm__ __volatile__("wbinvd" : : : "memory");

    // Врезаем транзакцию в асинхронный планировщик очередей ядра
    g_control_queue.qh.element = td0_phys & ~0x0Fu;
    __asm__ __volatile__("": : :"memory");

    uint32_t timeout = 5000000;
    int result = -3;

    // Читаем статус транзакции НАПРЯМУЮ ИЗ ПЛАНКИ ПАМЯТИ по физическому адресу
    volatile uint32_t *phys_status_ptr = (volatile uint32_t *)status_td_status_phys;

    while (timeout--)
    {
        uint32_t st = *phys_status_ptr;

        if (!(st & TD_CTRL_ACTIVE))
        {
            uint32_t bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR | TD_CTRL_CRCTIMEO;
            if (st & bad) result = -2;
            else          result = data_len; // УСПЕХ!
            break;
        }
    }

    // Восстанавливаем исходный пустой элемент очереди QH
    g_control_queue.qh.element = dummy_phys & ~0x0Fu;

    // Тотальный сброс кэша для синхронизации входящих данных флешки
    __asm__ __volatile__("wbinvd" : : : "memory");

    if (result > 0 && data_in && data_buf)
    {
        volatile uint8_t *cp = (volatile uint8_t *)data_buf;
        api_ptr->load_screen_add_string("USB DATA READ OK! Bytes: %x %x %x %x | %x %x %x %x",
                                        cp[0], cp[1], cp[2], cp[3], cp[4], cp[5], cp[6], cp[7]);
    }

    api_ptr->debug8 = control_tds[0].status;
    api_ptr->debug9 = control_tds[sidx].status;
    return result;
}






int uhci_bulk_transfer(uhci_hc_t *hc, uint8_t dev_addr, uint8_t ep_addr, uint16_t ep_max_pkt, uint8_t *toggle, void *buf, uint32_t len, int dir_in)
{
    if (!hc || !buf || len == 0 || !toggle) return -1;
    if (ep_max_pkt == 0 || ep_max_pkt > 1024) ep_max_pkt = 64;
    uint32_t num_tds = (len + ep_max_pkt - 1) / ep_max_pkt;
    if (num_tds > MAX_BULK_TDS)
    {
        num_tds = MAX_BULK_TDS;
        len = num_tds * ep_max_pkt;
    }
    uint8_t ep_num = ep_addr & 0x0F;
    uint8_t cur_toggle = *toggle ? 1 : 0;
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < num_tds; i++)
    {
        uint32_t off   = i * ep_max_pkt;
        uint32_t chunk = (len - off > ep_max_pkt) ? ep_max_pkt : (len - off);
        uint32_t toggle_bit = cur_toggle ? TD_TOKEN_TOGGLE : 0u;
        uint32_t next_td_phys = (i + 1 < num_tds) ? (usb_virt_to_phys((uint32_t)&bulk_tds[i + 1]) & ~0x0Fu) : TD_LINK_TERM;
        bulk_tds[i].link   = next_td_phys;
        bulk_tds[i].status = TD_CTRL_ACTIVE | (i == num_tds - 1 ? TD_CTRL_IOC : 0u);
        bulk_tds[i].token  = ((uint32_t)(dir_in ? PID_IN : PID_OUT) << TD_TOKEN_PID_SHIFT) | ((uint32_t)dev_addr << TD_TOKEN_ADDR_SHIFT) | ((uint32_t)ep_num << TD_TOKEN_EP_SHIFT) | toggle_bit | ((uint32_t)(chunk - 1) << TD_TOKEN_LEN_SHIFT);
        uint32_t pa = usb_virt_to_phys((uint32_t)(b + off));
        if (pa == 0) return -1;
        bulk_tds[i].buffer = pa;
        cur_toggle ^= 1;
    }
    uint32_t first_bulk_td_phys = usb_virt_to_phys((uint32_t)&bulk_tds[0]);
    uint32_t dummy_td_phys      = usb_virt_to_phys((uint32_t)&g_bulk_queue.dummy_td);
    if (!first_bulk_td_phys || !dummy_td_phys) return -1;
    bulk_tds[num_tds - 1].link = dummy_td_phys & ~0x0Fu;
    usb_dma_sync_before();
    g_bulk_queue.tail_td->link = first_bulk_td_phys & ~0x0Fu;
    g_bulk_queue.tail_td = &bulk_tds[num_tds - 1];
    uint32_t timeout = 5000000;
    int result = -3;
    while (timeout--)
    {
        uint32_t st = bulk_tds[num_tds - 1].status;
        if (!(st & TD_CTRL_ACTIVE))
        {
            uint32_t bad = TD_CTRL_STALLED | TD_CTRL_DBUFERR | TD_CTRL_BABBLE | TD_CTRL_CRCTIMEO;
            int all_ok = 1;
            for (uint32_t k = 0; k < num_tds; k++)
            {
                if (bulk_tds[k].status & bad)
                {
                    all_ok = 0;
                    break;
                }
            }
            if (all_ok)
            {
                *toggle = cur_toggle;
                result = (int)len;
            }
            else
            {
                result = -2;
            }
            break;
        }
    }
    usb_dma_sync_after();
    return result;
}
void uhci_register_ptrs(void)
{
    //frame_list_ptr    = frame_list;
    frame_list_ptr    = (uint32_t *)raw_frame_buffer;
    control_qh_ptr    = &control_qh;
    control_tds_ptr   = control_tds;
    setup_static_ptr  = setup_static;
    bulk_qh_ptr       = &bulk_qh;
    bulk_tds_ptr      = bulk_tds;
}
