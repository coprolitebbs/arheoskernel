# Драйвер файловой системы FAT12 (дискета 1.44/0.72 МБ)

Классический драйвер FAT12  контроллер флоппи-дисковода (FDC µPD765/8272) 
DMA-канал 2. Через FAT12 грузится загрузочная дискета; драйвер даёт полный
доступ на чтение и запись к дискетам из ОС (используется, например, для
копирования образов и проверки загрузочных дискет на реальном железе).

Исходники: `drivers/fat12_driver/`:
- `fat12_driver.c` — логика FAT12: тома, dirent, кластерные цепочки, файлы;
- `fat12_disk.c` — слой секторов: LBA→CHS, выбор FDC или block-API;
- `fat12_fdc.c` — работа с контроллером FDC (порты 0x3F2–0x3F7);
- `fat12_dma.c` — программирование DMA-канала 2;
- `fat12_boot.c` — чтение boot-сектора и разбор BPB;
- `fat12_start.asm` — заголовок DRV1 и точка входа.

## Схема работы

```
     Ядро / VFS / syscalls
             │  fs_apis[0].api  (FS_FAT12)
             v
   ┌────────────────────────────── fat12_driver.c ────────────────────────┐
   │  Тома:  fat12_volumes[8]            Файлы: fat12_files[16]           │
   │  fat12_fs_t { boot_sector, bytes_per_sector, sectors_per_cluster,    │
   │               root_start, data_start, sectors_per_track, heads,      │
   │               fat[8192]  ←— вся FAT в памяти }                       │
   │                                                                      │
   │  read(fd): position → кластер в цепочке → LBA → сектор               │
   │  write(fd): выделение кластера из FAT, дозапись dirent               │
   └──────────────┬───────────────────────────────────────────────────────┘
                  │ fat12_read_sectors(lba, count)
   ┌──────────────┴──────────────── fat12_disk.c ─────────────────────────┐
   │  use_block_api?  → block->read(drive, lba, n, buf)                   │
   │  иначе (флоппи):                                                     │
   │    lba_to_chs(lba) → {cylinder, head, sector}                        │
   └──────────────┬───────────────────────────────────────────────────────┘
                  │ fdc_read_sector(c, h, s)
   ┌──────────────┴───────────────── fat12_fdc.c ─────────────────────────┐
   │  DOR 0x3F2: моторDMAIRQ, выбор привода                             │
   │  FIFO 0x3F5: READ DATA (0x46|MT) с параметрами C/H/S                 │
   │  Ожидание IRQ6: ядро ставит floppy_irq_fired = 1                     │
   │  Результат: SENSE INTERRUPT  7 байт статуса                         │
   └──────────────┬───────────────────────────────────────────────────────┘
                  │ параллельно
   ┌──────────────┴───────────────── fat12_dma.c ─────────────────────────┐
   │  Канал 2 ISA DMA: MASK 0x0A, MODE 0x0B, ADDR 0x04, COUNT 0x05,       │
   │  PAGE 0x81; буфер фикс. адреса FAT12_DMA_BUFFER = 0x00070000         │
   └──────────────────────────────────────────────────────────────────────┘
```

## Аппаратная часть

### Контроллер FDC (µPD765-совместимый)

| Порт | Регистр | Роль |
|---|---|---|
| 0x3F2 | DOR (write) | выбор привода, мотор, сброс, разрешение DMA/IRQ |
| 0x3F4 | MSR (read) | статус: RQM (можно писать), DIO, CB (занят) |
| 0x3F5 | FIFO (r/w) | байты команд и результатов |
| 0x3F7 | CCR (write) | конфигурация скорости; в коде объявлен, но не используется — работает скорость по умолчанию |

Используемые команды: `SPECIFY` (0x03), `RECALIBRATE` (0x07),
`SENSE INTERRUPT` (0x08), `SEEK` (0x0F), `READ DATA` (0x46|0x40 MT),
`WRITE DATA` (0x45|0x40 MT).

Последовательность чтения сектора (`fdc_read_sector`):

1. Флаг IRQ6 сбрасывается, DMA настраивается на приём 512 байт.
2. `outb(DOR, ENABLE|DMA_IRQ|MOTOR0|DRIVE0)` — раскрутить мотор, включить IRQ.
3. В FIFO подаются 9 байт команды READ DATA: код, `head<<2`, C, H, S,
   размер (2 = 512), секторов на дорожке, GPL 0x1B, DTL 0xFF.
4. Ожидание завершения: флаг `floppy_irq_fired`, выставляемый ядром
   из обработчика IRQ6 (указатель `floppy_irq_fired_ptr` внедряется ядром
   при загрузке драйвера), плюс таймаут на MSR.CB.
5. Чтение 7 байт результата; `ST0 & 0xC0` — ошибка → `FS_IO_ERROR`.
6. Данные вычитываются из DMA-буфера 0x70000 в буфер вызывающего.

Запись симметрична: DMA настраивается на передачу, буфер сначала копируется
в 0x70000, подаётся WRITE DATA, после IRQ6 читается статус.

### ISA DMA, канал 2

Классическое программирование контроллера DMA:

```
 MASK 0x0A ← 0x06        канал 2 замаскирован
 FLIP-FLOP 0x0C ← 0      сброс триггера байт-порядка
 ADDR 0x04 ← 0x70000     адрес буфера (16 бит)
 PAGE 0x81 ← 0x00        A16..A23 (page register)
 COUNT 0x05 ← 511        число байт − 1
 MODE 0x0B ← single/чтение-или-запись/канал2
 MASK 0x0A ← 0x02        размаскировать → DMA готов
```

Буфер DMA фиксированный (`FAT12_DMA_BUFFER = 0x00070000`, 512 байт) —
контроллер ISA DMA умеет адресовать только первые 16 МБ физической памяти,
поэтому все пересылки идут через эту промежуточную зону.

## Монтирование

`fat12_mount(drive)`:

1. Свободный слот тома (до `FAT12_MAX_VOLUMES = 8`).
2. `fat12_read_boot_sector()` — сектор 0.
3. `fat12_parse_bpb()` — разбор BPB: bytes_per_sector, sectors_per_cluster,
   reserved_sectors, fat_count, sectors_per_fat, root_entries,
   sectors_per_track, heads. Вычисляются `root_start`, `root_size`,
   `data_start`.
4. **Вся первая FAT целиком загружается в память** (`vol->fat`, лимит
   `FAT12_MAX_FAT_SIZE = 8192` байт — достаточно для 1.44/0.72 МБ).
   Все операции с кластерными цепочками (`fat12_get_next_cluster`,
   `fat12_set_cluster_value`) работают по этой копии; после изменений FAT
   сбрасывается обратно `fat12_flush_fat()`.
5. Том помечается `mounted = 1`.

## Операции ФС

| Функция | Что делает |
|---|---|
| `mount / unmount / get_volume` | тома (размонтирование запрещено при открытых файлах) |
| `open / close / read / write` | файлы, до `FAT12_MAX_OPEN_FILES = 16` |
| `seek / tell / stat` | позиция, атрибуты и время (DOS→UNIX конверсия) |
| `readdir` | корень и подкаталоги; поддержка метки тома и `0xE5` (удалённые) |
| `lookup / touch` | поиск 8.3-имени в каталоге, создание пустого файла |
| `mkdir / rmdir / unlink / rename` | полный набор операций с каталогами |
| `statfs` | общий размер/свободное место в байтах |

Особенности реализации:

- имена преобразуются в 8.3-форму `fat12_make_83()` (прописные, точка
  выравнивается на позицию 8);
- запись дозаписывает кластеры: `fat12_allocate_cluster()` ищет свободный
  кластер в FAT-копии, сшивает цепочку (EOF-метка 0xFFF), обновляет dirent
  (`fat12_update_dirent`) и размер файла;
- удаление (`unlink`/`rmdir`) очищает цепочку кластеров
  (`fat12_free_chain`) и первый байт dirent → 0xE5.

## Выбор носителя: FDC или block-API

`fat12_read_sectors()`/`fat12_write_sectors()` сначала проверяют
`vol->use_block_api`: если флаг взведён и block-драйвер доступен, секторы
идут через `block->read/write` — так драйвер может обслуживать FAT12 не
только на физической дискете. По умолчанию (флоппи-носитель) используется
связка FDC  DMA  IRQ6, описанная выше.

Долгие дисковые операции также идут с учётом `schedule_lock_ptr`
(`KSL_ACQUIRE/KSL_RELEASE`) — запрещено вытеснение в середине транзакции
с контроллером.

## Интеграция с ядром

- Загружается четвёртым (`boot_info->modules[3]`), регистрируется как
  `fs_apis[0]` c типом `FS_FAT12`.
- Ядро внедряет: `floppy_irq_fired_ptr = &floppy_irq_fired` (сигнал от
  обработчика IRQ6), `schedule_lock_ptr`, `malloc_ptr`, `free_ptr`, `block`.
- Тот же указатель `floppy_irq_fired_ptr` получает ext2-драйвер — он нужен
  обоим, т.к. ядро ставит флаг по IRQ6 независимо от того, какой ФС-драйвер
  его ждёт.

## Сборка

```
gcc  -m32 -march=i386 -ffreestanding -fno-common -fno-stack-protector
     -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie
     -c drivers/fat12_driver/{fat12_driver,fat12_disk,fat12_boot,
                              fat12_fdc,fat12_dma}.c -o bin/...
nasm -f elf32 drivers/fat12_driver/fat12_start.asm -o bin/fat12_start.o
ld -m elf_i386 --no-warn-rwx-segments --emit-relocs -T drivers/userspace_driver.ld \
   bin/fat12_start.o bin/fat12_driver.o bin/fat12_disk.o bin/fat12_boot.o \
   bin/fat12_fdc.o bin/fat12_dma.o bin/drv_ports_io.o bin/dlib.o \
   -o bin/fat12_driver.elf
```

Загружается как модуль 3 (`boot_info->modules[3]`).

# arheoskernel