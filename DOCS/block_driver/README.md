# Драйвер блоковых устройств (ATA PIO)

Блоковый драйвер — фундамент дисковой подсистемы ARHeOS. Он:

- обнаруживает ATA-диски на первичном и вторичном каналах IDE-контроллера,

- читает/пишет секторы в режиме PIO с адресацией LBA28,

- ведёт реестр устройств (`sda`, `sdb`, ...),

- предоставляет **единый block-API**, через который работают драйверы
  файловых систем (ext2, FAT12) и через который USB-драйвер регистрирует
  свои накопители (USB flash stick становится «ещё одним диском»).

Исходники: `drivers/block_driver/` (`block_driver.c`, `block_start.asm`, `include/block_driver.h`).

## Схема работы

```
                ЯДРО (use_drivers.c)                     Клиенты block-API
              load_driver_elf(module[1])               ┌────────────────────┐
                       │                               │ ext2_driver        │
                       v                               │ fat12_driver       │
             block_driver_api_t *blockdevice           │ usb_driver (MSC)   │
                       │                               └─────────┬──────────┘
                       │  block->read(id, lba, n, buf)           │
                       │  block->write(id, lba, n, buf)          │
                       v                                         v
        ┌──────────────────────────────────────────────────────────────────┐
        │  РЕЕСТР УСТРОЙСТВ  block_dev_entry_t devices[16]                 │
        │  ┌──────┬──────────────┬───────────┬───────────────────────────┐ │
        │  │ id=0 │ "sda"        │ ATA PIO   │ read_fn=ata_read_wrap     │ │
        │  │ id=1 │ "sdb"        │ ATA PIO   │ priv = ata_priv_t         │ │
        │  │ id=2 │ "usb0"       │ USB MSC   │ read_fn из USB-драйвера   │ │
        │  └──────┴──────────────┴───────────┴───────────────────────────┘ │
        └──────────────────────────────┬───────────────────────────────────┘
                                       │ dispatch по id
                       ┌───────────────┴────────────────┐
                       v                                v
              ata_read_wrap()                    usb_msc_read_wrap()
              (priv = {base, slave})             (priv = usb_msc_dev_t)
                       │                                │
                       v                                v
             ATA PIO порты 0x1F0/0x170         USB BOT: CBW/CSW по bulk-EP
```

Ключевая идея: block-драйвер — это не только сам ATA. Его функция
`register_device()` позволяет **чужому** драйверу (USB) добавить устройство
со своими функциями чтения/записи. Дальше все потребители работают
одинаково — по числовому `dev_id`, не зная, диск это или флешка.

## Обнаружение дисков

`block_init()` последовательно опрашивает четыре позиции IDE:

```
 ata_probe(0x1F0, 0);  // primary master
 ata_probe(0x1F0, 1);  // primary slave
 ata_probe(0x170, 0);  // secondary master
 ata_probe(0x170, 1);  // secondary slave
```

Каждый `ata_probe()` выполняет команду `IDENTIFY DEVICE` (0xEC):

1. Если канал «залип» (BSY держится) — программный сброс `ata_soft_reset()`
   через control-порт (0x3F6 / 0x376): SRST=1 → пауза → SRST=0.
2. Если от предыдущей команды остался непрочитанный DRQ — буфер вычитывается
   (256 `inw`), чтобы не сбить состояние контроллера.
3. Выбор устройства (0xA0/0xB0 на порт 0x1F6), обнуление регистров, команда 0xEC.
4. Если `status == 0x00/0xFF` — диска нет. Если LBA Mid/High != 0 — это ATAPI
   (CD-ROM), драйвер такие устройства не обслуживает.
5. Ожидание DRQ, чтение 256 слов identify-данных.

Из identify берётся ёмкость:

- слова 60/61 — число секторов LBA28 (основной источник);
- если 0 — слова 100/101 (LBA48, «только для информации»: чтение всё равно
  идёт через LBA28).

Устройству присваивается имя `sd`  буква (`sda`, `sdb`, ... по счётчику
найденных дисков) и оно заносится в реестр.

## Чтение и запись (ATA PIO)

Режим: PIO, строго по 1 сектору за итерацию, LBA28.

```
 ata_pio_read(base, slave, lba, count, buf)      ata_pio_write(...)
        |                                               |
 outb(0x1F6, 0xE0|drive|((lba>>24)&0x0F))  -- выбор диска  старшие 4 бита LBA
 4x inb(0x1F7)                             -- пауза >=400 нс для старых чипов
 outb(0x1F2, 1)                            -- считать 1 сектор
 outb(0x1F3..0x1F5)                        -- LBA 0..23
 outb(0x1F7, 0x20)  READ SECTORS           -- 0x30 WRITE SECTORS для записи
        |
 опрос статуса 0x1F7: ждать BSY=0 && DRQ=1; ERR (бит 0) -> FS_IO_ERROR
        |
 insw(0x1F0, buf512*i, 256)               -- outsw(0x1F0, ...) для записи
        |                                     после записи - ожидание BSY=0
 повторить count раз
```

Перед каждой строковой операцией портов выполняется `cld` (сброс DF) —
гарантия направления `insw/outsw` независимо от состояния флагов вызывающего кода.

Обёртки `ata_read_wrap()`/`ata_write_wrap()` (сигнатура с `void *priv`)
привязывают устройство к его приватным данным `ata_priv_t {base, slave}`
и отсекают LBA > 4 ГБ (граница LBA28).

## Реестр устройств и внешний API

Запись реестра:

```
typedef struct {
    char     name[16];        // "sda", "usb0", ...
    uint32_t sector_count;    // ёмкость в секторах 512
    uint32_t flags;           // BLK_REMOVABLE / BLK_READONLY / BLK_PARTITION
    bool     used;
    int (*read_fn) (void*, uint64_t lba, uint32_t n, void*buf);        // внешние
    int (*write_fn)(void*, uint64_t lba, uint32_t n, const void*buf);  // функции
    void    *priv;            // контекст драйвера-владельца
} block_dev_entry_t;
```

Максимум `MAX_BLOCK_DEVICES = 16` устройств, размер сектора фиксирован —
`SECTOR_SIZE = 512`.

API-структура `block_driver_api_t` (поле `struct_ptr` в `description_driver_api_t`,
тип `DESCR_BLOCKDEVICE_DRIVER`):

| Функция | Назначение |
|---|---|
| `read(dev_id, lba, count, buf)` | чтение секторов; проверка id и `used` |
| `write(dev_id, lba, count, buf)` | запись; `BLK_READONLY` → `FS_ACCESS_DENIED` |
| `get_info(dev_id, block_info_t*)` | имя, размер, секторы, флаги |
| `enum_devices(index, block_info_t*)` | перечисление по индексу 0..N-1 |
| `find_by_name("sda", &id)` | поиск по имени (для mount-by-name из userspace) |
| `get_partition / scan_partitions` | пока `FS_NOT_SUPPORTED` (заготовка под MBR/GPT) |
| `resolve_boot_drive(bios_drive, &id)` | BIOS-диск `0x80k` → id реестра |
| `register_device(name, sectors, flags, read_fn, write_fn, priv)` | регистрация внешнего устройства, возвращает новый id |
| `malloc_ptr / free_ptr / yield_ptr` | внедряются ядром после загрузки |

Особенность `resolve_boot_drive()`: порядок дисков у BIOS может не совпасть
с ATA-порядком (типичный случай — загрузка с USB, когда ATA-дисков нет вообще).
Тогда индекс вне диапазона корректно отображается в id 0, а точное совпадение
позже поправит USB-драйвер, зарегистрировав флешку первым устройством.

## Интеграция с ядром

- Загружается **вторым** (`boot_info->modules[1]`) в `drivers_init()`.
- После загрузки ядро внедряет `malloc_ptr`/`free_ptr`/`yield_ptr` и проверяет
  `enabled` (true, если найден хотя бы один диск).
- Указатель на API передаётся дальше: FS-драйверам (`fs_apis[i].api->block`)
  и USB-драйверу (`usb_driver_api->block`) — так они получают доступ к диску.
- ФС-драйвер вызывает `block->read(drive_id, lba, count, buf)`, где `drive_id` —
  id в реестре block-драйвера.

## Сборка

```
gcc  -m32 -march=i386 -ffreestanding -fno-common -fno-stack-protector
     -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie
     -c drivers/block_driver/block_driver.c -o bin/block_driver.o
nasm -f elf32 drivers/block_driver/block_start.asm -o bin/block_start.o
ld -m elf_i386 --no-warn-rwx-segments --emit-relocs -T drivers/userspace_driver.ld \
   bin/block_start.o bin/block_driver.o bin/drv_ports_io.o bin/dlib.o \
   -o bin/block_driver.elf
```

ELF укладывается в образ сразу после ядра и подхватывается загрузчиком как
модуль 1; его адрес и размер попадают в `boot_info->modules[1]`.

# arheoskernel