# USB-стек (UHCI / EHCI / Mass Storage)

Драйвер USB для ARHeOS: находит хост-контроллеры через PCI, инициализирует
их (UHCI  EHCI), перечисляет устройства (включая вложенные хабы),
подключает флешки как Mass Storage и регистрирует их в блок-слое — после
чего флешка для системы неотличима от обычного диска. Поддержка
Panasonic CF-18 (ICH6-M: один EHCI  companion UHCI) — одно из целевых
желез проекта.

Исходники: `drivers/usb_driver/`:

| Файл | Роль |
|---|---|
| `usb_driver.c` | точка входа, перечисление устройств, регистрация MSC в block-слое |
| `usb_core.c` | общие протоколы: control/bulk-обёртки, дескрипторы, SET ADDRESS/CONFIG, хабы, parse_mass_storage |
| `usb_uhci.c` | хост-контроллер UHCI: PCI, порты, frame list, QH/TD, control/bulk |
| `usb_ehci.c` | хост-контроллер EHCI: MMIO, BIOS handoff, QH/QTD, control/bulk |
| `usb_msc.c` | Mass Storage: BOT (CBW/CSW)  SCSI READ10/WRITE10 |
| `usb_start.asm` | заголовок DRV1 и точка входа |

## Схема работы

```
                        ЯДРО (use_drivers.c, drivers_init)
                                      │ load_driver_elf(module[2])
                                      v
                     usb_stack_driver_api_t *usb_driver_api
                     (внедрены: block, schedule_lock_ptr,
                      dma_alloc/dma_free, vmm_map_pages_uncached_ptr,
                      malloc/free, get_ticks, load_screen_add_string)
                                      │
                                      │ usb_start() == device_usb_start()
                                      v
 ┌──────────────────────────── usb_driver.c ─────────────────────────────┐
 │ 1. uhci_alloc_structures(), ehci_alloc_structures()  ← DMA-пул        │
 │ 2. usb_scan_pci_controllers()   (0xCF8/0xCFC, class 0C/03)            │
 │ 3. EHCI сначала: ehci_init → BIOS handoff → порты →                   │
 │    usb_enum_device_at() на каждый подключённый порт                   │
 │ 4. UHCI (companion) затем: uhci_init → порты 1..2 →                   │
 │    usb_enum_device_at()                                               │
 │ 5. Каждое устройство: usb_setup_device()                              │
 │    ├─ запись в devices[] → потом ядро перекладывает в usb_registry    │
 │    ├─ если Mass Storage (class 08):                                   │
 │    │    usb_msc_init() → INQUIRY, READ CAPACITY                       │
 │    │    block->register_device("usbN", sectors, BLK_REMOVABLE,        │
 │    │              usb_msc_read_wrap, usb_msc_write_wrap, slot)        │
 │    └─ если хаб (class 09): рекурсивный обход портов (depth < 5)       │
 └───────────────────────────────────────────────────────────────────────┘
                                      │
                    результаты видны системе:
                    block id "usb0" (block_driver)  usb_registry (userspace)
```

## Общий слой (usb_core.c)

Работа с устройством не зависит от контроллера — контроллеры прячутся за
обёрткой `usb_hc_t {type: USB_HC_UHCI|USB_HC_EHCI, hc: ...}`:

```
 usb_hc_control_transfer / usb_hc_bulk_transfer / usb_hc_port_*  → dispatch
```

Поверх этого реализованы стандартные USB-процедуры:

- `usb_get_device_desc / usb_get_config_desc` — GET_DESCRIPTOR;
- `usb_set_address` — SET_ADDRESS (адрес = `1  device_count`);
- `usb_set_configuration` — SET_CONFIGURATION;
- `usb_hub_probe / usb_hub_port_connected / usb_hub_port_reset` — работа
  с хабами (class 09);
- `usb_parse_mass_storage` — поиск интерфейса
  class 08 / subclass 06 (SCSI) / protocol 50 (BOT), выбор bulk-эндпоинтов
  IN и OUT;
- `usb_enumerate` — полный цикл подключения нового устройства.

## Контроллеры

### UHCI (Intel, progif 0x00)

- Поиск через PCI config (0xCF8/0xCFC): class 0x0C, subclass 0x03,
  progif 0x00; бар I/O берётся из BAR4.
- Инициализация `uhci_init`: HC reset (`USBCMD_HCRESET`), глобальный
  GRESET, сборка структуры расписания: **frame list** (1024 записей,
  физически непрерывный из DMA-пула), очередь Control QH и Bulk QH,
  статические пулы TD.
- `uhci_control_transfer` собирает цепочку TD (SETUP → DATA → STATUS)
  под общий Control QH и ждёт её завершения опросом статуса с таймаутом
  по `get_ticks()`.
- `uhci_bulk_transfer` — то же для bulk-цепочки (данные MSC).
- Порты: `PORTSC1/PORTSC2` (0x10/0x12 от базы); reset порта
  (PORTSC_PR) с ожиданием включения (PED). LS-устройства отмечаются
  в `uhci_dev_is_ls[]` (низкоскоростной префикс TD).

### EHCI (Intel, progif 0x20)

- BAR0 → MMIO; `CAPLENGTH` → база операционных регистров; количество
  портов из HCSPARAMS.
- **BIOS handoff** (`ehci_bios_handoff`): до начала работы нужно забрать
  шину у BIOS/legacy — запись в PCI config `USBLEGSUP` (OS Owned),
  ожидание освобождения, затем `USBCMD_CF = 1` (`ehci_take_ports`).
- Асинхронный список: head QH  пул QH control  bulk QH, периодический
  frame list; передача — цепочка qTD (SETUP/DATA/STATUS) c bounce-буферами
  (`ctl_bounce_ptr`/`bulk_bounce_ptr`) из DMA-пула.
- `ehci_dev_speed[]` хранит скорость устройств, подключённых через
  companion-схему.

**Порядок инициализации EHCI-прежде-UHCI не случаен**: на реальном железе
(CF-18) пока EHCI владеет шиной, фоновый трафик UHCI-companion («фантомные»
LS-устройства на общих портах) не может вмешаться в перечисление и
инициализацию MSC. UHCI-устройства опрашиваются после и уже ничему не мешают.

## Mass Storage (usb_msc.c)

Протокол Bulk-Only Transport (BOT):

```
 usb_msc_init:      INQUIRY → (SCSI) → READ CAPACITY(10) → sector_count/size
 usb_msc_read10:    CBW 'USBC' (0x43425355) → DATA-IN → CSW 'USBS' (0x53425355)
 usb_msc_write10:   CBW → SCSI WRITE10 → DATA-OUT → CSW
```

- CBW/CSW идут по bulk-эндпоинтам, тег `msc_tag` инкрементируется,
  сверяется в CSW; `bCSWStatus != 0` → ошибка.
- Восстановление после ошибок: `usb_msc_clear_halt` (CLEAR FEATURE HALT)
  и `usb_msc_bot_reset` (Bulk-Only Mass Storage Reset).

## Регистрация устройства

`usb_setup_device()`:

1. Каждое найденное устройство (любого класса) попадает в
   `devices[USB_REGISTRY_MAX = 8]` (vid, pid, class, subclass, protocol,
   port). После `usb_start()` ядро перекладывает записи в глобальный
   `usb_registry` (`usb_registry.c`) — их читает userspace
   (`udevices_struct.h`, syscall-перечисление устройств).
2. MSC-устройства дополнительно:
   - занимают слот `g_msc[USB_MAX_MSC_DEVS = 4]`;
   - `usb_msc_init()` определяет ёмкость;
   - `block->register_device("usbN", sector_count, BLK_REMOVABLE,
     usb_msc_read_wrap, usb_msc_write_wrap, slot)` — USB-драйвер
     **становится клиентом block-драйвера**: его обёртки по `slot`
     выполняют READ10/WRITE10. Возвращённый `block_id` сохраняется
     и в записи реестра (`0xFFFFFFFF` = не блок-девайс).
3. `hc_wrap` копируется в слот: указатель на контроллер на стеке умирает,
   а устройству нужен постоянный контекст.

## DMA-память

Контроллеры обращаются к структурам (QH/TD/буферы) по **физическим**
адресам, поэтому все они выделяются через внедрённый ядром `dma_alloc_ptr`:

- ядро (`kernel_usb_dma_alloc`) берёт физически непрерывные страницы
  (`pmm_alloc_pages`), мапит их uncached (`vmm_map_pages_uncached`) и
  сбрасывает кэш (`wbinvd`);
- для x86 identity-области virt == phys, но драйвер сознательно не
  полагается на это и всегда переводит адреса через
  `usb_virt_to_phys()`;
- барьеры памяти `uhci_wmb/mb/rmb` расставлены перед передачей
  структур контроллеру.

## Отладка

- каждый шаг перечисления печатается на загрузочный экран через
  `load_screen_add_string` (`EHCI found %d controller(s)`,
  `EHCI %d p%d PORTSC=%x`, `USB msc addr=%d reg %s -> %d`, ...);
- `debug1..debug9` в API — контрольные точки состояния портов;
- `last_error`: 0xBAD3 — устройств не найдено, 0xBAD6/0xBAD7 — не хватило
  DMA-структур UHCI/EHCI;
- `usb_driver-not-working/` — архивная ранняя версия стека (отдельный
  ehci_core/usb_hcd/usb_hub), сохранена для истории, не собирается.

## Интеграция с ядром

- Загружается третьим (`boot_info->modules[2]`), сразу после block-драйвера.
- Ядро внедряет: `schedule_lock_ptr`, `block = blockdevice_driver`,
  `dma_alloc_ptr/dma_free_ptr`, `vmm_map_pages_ptr/vmm_map_pages_uncached_ptr`,
  `malloc_ptr/free_ptr`, `get_ticks`, `load_screen_add_string`.
- Затем ядро вызывает `usb_start()` и переносит `devices[]` в `usb_registry`.

## Сборка

```
gcc  -m32 -march=i386 -ffreestanding -fno-common -fno-stack-protector
     -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie
     -c drivers/usb_driver/{usb_core,usb_uhci,usb_msc,usb_ehci,usb_driver}.c \
        -o bin/...
nasm -f elf32 drivers/usb_driver/usb_start.asm -o bin/usb_start.o
ld -m elf_i386 --no-warn-rwx-segments --emit-relocs -T drivers/userspace_driver.ld \
   bin/usb_start.o bin/usb_core.o bin/usb_uhci.o bin/usb_msc.o bin/usb_ehci.o \
   bin/usb_driver.o bin/drv_ports_io.o bin/dlib.o -o bin/usb_driver.elf
```

Загружается как модуль 2 (`boot_info->modules[2]`).

# arheoskernel