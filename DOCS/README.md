ARHeOS — документация драйверов

Эта директория содержит документацию по драйверам ARHeOS: принцип работы каждого
драйвера, схема взаимодействия с ядром, используемые аппаратные ресурсы и API.

## Индекс документации

| Драйвер | Документ | Исходники | Тип API |
|---|---|---|---|
| Фреймбуфер (рисование, шрифт) | [framebuffer/README.md](framebuffer/README.md) | `drivers/fb_driver/` | `fb_driver_api_t` |
| Блоковые устройства (ATA PIO) | [block_driver/README.md](block_driver/README.md) | `drivers/block_driver/` | `block_driver_api_t` |
| Файловая система ext2 | [ext2_driver/README.md](ext2_driver/README.md) | `drivers/ext2_driver/` | `fs_driver_api_t` |
| Файловая система FAT12 (флоппи) | [fat12_driver/README.md](fat12_driver/README.md) | `drivers/fat12_driver/` | `fs_driver_api_t` |
| Клавиатура PS/2 (i8042) | [kbd_driver/README.md](kbd_driver/README.md) | `drivers/kbd_driver/` | `kbd_driver_api_t` |
| USB-стек (UHCI/EHCI/MSC) | [usb_driver/README.md](usb_driver/README.md) | `drivers/usb_driver/` | `usb_stack_driver_api_t` |
| Загрузчик (stage2 → boot_info) | [loader/README.md](loader/README.md) | `f12ld.asm`, `hddld.asm`, `bootinfo.c` | — |

Общий заголовок ABI всех драйверов: [`drivers/include_drivers/drv_format.h`](../drivers/include_drivers/drv_format.h).

---

## Единый формат драйвера DRV1

Каждый драйвер собирается в позиционно-независимый relocatable-ELF (i386) со
специальным заголовком `DRV1` в начале файла. Формат описан в
`drv_format.h` и `*start.asm`.

Схема сборки одного драйвера:

```
  .c-файлы драйвера        *.asm (NASM)          общие объекты
  ┌────────────────┐      ┌────────────────┐     ┌──────────────────┐
  │ xxx_driver.c   │      │ xxx_start.asm  │     │ lib.c    (dlib.o)│
  │ ...            │      │  DRV1-заголовок│     │ ports_io.c       │
  └───────┬────────┘      └───────┬────────┘     └────────┬─────────┘
          │  gcc -m32 -ffreestanding -fno-pie             │
          └──────────────┬────────────────────────────────┘
                         v
        ld -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld
                         v
                 bin/xxx_driver.elf   → диск (образ) → модуль загрузчика
                                      или /adrv/xxx.elf (динамическая загрузка)
```

Точка входа драйвера:

```
driver_entry (asm)
   |-- заголовок 'DRV1' (magic, version, размер, оффсет entry, имя)
   `--> driver_main(struct boot_info *boot)      ; EBX = boot_info
            |
            |- заполняет статическую структуру своего API (например fs_driver_api_t)
            `- выставляет description_driver_api_t:
                   type       = DESCR_*_DRIVER   (тип драйвера)
                   struct_ptr = &api             (указатель на API-структуру)
```

Идентификаторы типов (`drv_format.h`):

```
DESCR_FRAMEBUFFER_DRIVER   1
DESCR_FILESYSTEM_DRIVER    2
DESCR_KEYBOARD_DRIVER      3
DESCR_BLOCKDEVICE_DRIVER   4
DESCR_USB_DRIVER           5
```

## Как ядро загружает драйверы

Загрузка выполняется `load_driver_elf()` (`driver_elf.c`): секции .text/.rodata/.data
копируются в память, .bss обнуляется, обрабатываются релокации `R_386_32` и
`R_386_PC32`, затем ищется символ `driver_get_api()` и возвращается указатель на
`description_driver_api_t`.

Есть два канала загрузки:

1. **Модули загрузчика** (`boot_info->modules[]`) — обязательные драйверы,
   уложенные на диск сразу после ядра ещё на этапе сборки образа.
2. **Каталог `/adrv/`** — дополнительные драйверы, перечисленные в конфиге
   `/config/boot/boot.cfg` (секции `[driver_1]`, `[driver_2]`, ... ,
   ключ `name`). Загружаются `load_additional_drivers()` уже через VFS,
   так клавиатура попадает в систему.

Порядок инициализации (`use_drivers.c :: drivers_init()`):

```
 module[0]  fb_driver     ── fb_driver_api_t*  fb            (рисование)
 module[1]  block_driver  ── block_driver_api_t* blockdevice (ATA-диски)
 module[2]  usb_driver    ── usb_stack_driver_api_t* usb
 module[3]  fat12_driver  ── fs_apis[0]  (FS_FAT12)
 module[4]  ext2_driver   ── fs_apis[1]  (FS_EXT2)
 /adrv/...  kbd_driver    ── kbd_driver_api_t* kbd_driver_api
```

При загрузке каждому драйверу ядро внедряет указатели на свои функции —
драйверы никогда не линкуются с ядром напрямую:

```
 api->malloc_ptr   = kmalloc          api->free_ptr      = kfree
 api->yield_ptr    = sys_yield        api->get_ticks     = tick_count
 api->schedule_lock_ptr = &kernel_schedule_lock   (запрет schedule() в IRQ)
 api->block        = blockdevice_driver (для FS/USB-драйверов)
 api->floppy_irq_fired_ptr = &floppy_irq_fired  (сигнал IRQ6 для FAT12)
 api->dma_alloc_ptr / dma_free_ptr           (физически непрерывные страницы)
 api->vmm_map_pages_uncached_ptr             (MMIO/девайсная память)
 api->load_screen_add_string                 (отладка на загрузочном экране)
```

Общая схема загрузки:

```
 BIOS/загрузчик stage2
        |
        |-- FAT12/ext2: читает ядро и модули драйверов с диска
        |-- строит filesystem_info_t, module_info_t[], boot_info_t
        v
      ЯДРО  kernel.c → drivers_init()  (use_drivers.c)
        |
        |-- load_driver_elf_mem() по очереди для каждого модуля
        |-- проверяет description_driver_api_t->type
        |-- внедряет kernel-указатели в API-структуру
        |-- usb_start() → перечисление USB, MSC регистрируется в block-слое
        |-- mount() ФС-драйверов → VFS
        `-- load_additional_drivers() → /adrv/ (клавиатура) → IRQ1 hook
```

## Правила, общие для всех драйверов

- Драйвер работает в Ring 0 и вызывается и из задач, и из обработчиков IRQ;
  длительные дисковые операции обязаны брать `KSL_ACQUIRE(schedule_lock_ptr)`
  и отпускать его через `KSL_RELEASE` — см. макросы в `drv_format.h`.
- Память выделяется только через `malloc_ptr`/`free_ptr` (kernel heap),
  DMA-память — только через `dma_alloc_ptr` (физически непрерывные,
  uncached-страницы).
- Аппаратные порты читаются/пишутся через функции `inb/outb/insw/outsw`
  из `ports_io.c` (объект `drv_ports_io.o` линкуется с каждым драйвером).
