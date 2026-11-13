# Драйвер файловой системы ext2

Полноценный RW-драйвер ext2 (revision 0/1) для ARHeOS. Через него работает
корневая ФС жёсткого диска (`hdd_ext2.img`): от него зависят спавн программ,
`ls`, конфиги загрузки и вся пользовательская работа с файлами.

Исходники: `drivers/ext2_driver/`:
- `ext2_driver.c` — логика ФС: тома, иноды, каталоги, файловые операции;
- `ext2_disk.c` — дисковый слой: блоки, иноды на диске, битовые карты аллокации;
- `ext2_start.asm` — заголовок DRV1 и точка входа;
- `include/` — структуры on-disk формата (`ext2_internal.h`) и прототипы.

## Схема работы

```
      Ядро / VFS / syscalls                          Userspace ("ls -l")
             │  fs_apis[1].api  (FS_EXT2)                    │
             v                                               │ open/read/write
   ┌─────────────────────────────── ext2_driver.c ───────────┴───────────┐
   │  Тома:  ext2_volumes[10]        Файлы: ext2_files[16]               │
   │  ext2_volume_t {superblock, BGDT, block_size,                       │
   │                 partition_start_lba, inode_size, groups_count}      │
   │                                                                     │
   │  open(path) ─► lookup по dirent ─► ext2_read_inode ─► ext2_file_t   │
   │  read(fd):  position → file_block_index → phys_block                │
   │             (12 прямых  3 уровня косвенности)                       │
   └──────────────────────────────┬──────────────────────────────────────┘
                                  │ ext2_read_block / ext2_write_block
   ┌──────────────────────────────┴────────────── ext2_disk.c ────────────┐
   │  LBA = partition_start_lba  block * sectors_per_block                │
   │  read_inode:  группа = (ino-1) / inodes_per_group                    │
   │               таблица инодов из BGDT, (ino-1) % ipg = смещение       │
   │  alloc/free: битовые карты блоков и инодов группы  счётчики          │
   └──────────────────────────────┬───────────────────────────────────────┘
                                  │ KSL_ACQUIRE(schedule_lock)
                 ┌────────────────┴─────────────────┐
                 v                                  v
    block->read(drive, lba, n, buf)      ext2_disk_read_sectors()
    (блоковый драйвер: ATA/USB-MSC)      (прямая ATA PIO 0x1F0 — fallback)
```

## Монтирование

`ext2_mount(drive)` (driving `drive` — id устройства в block-слое):

1. Ищет свободный слот тома (до `EXT2_MAX_VOLUMES = 10`); структура тома
   выделяется через `malloc_ptr` (kernel heap) — драйвер полностью изолирован
   от других томов.
2. **Поиск суперблока**: сканирует LBA-офсеты раздела 0..64. Суперблок ext2
   всегда лежит на смещении 1024 байта от начала ФС, поэтому читаются 2 сектора
   на `scan_lba  2`. Валидация: `s_magic == 0xEF53`, `s_log_block_size <= 4`,
   разумные `s_blocks_count` / `s_inodes_per_group`, `s_block_group_nr == 0`.
   Найденный LBA запоминается в `vol->partition_start_lba` (работает и для
   раздела с «пустым» MBR, и для сырого образа).
3. Вычисляется геометрия: `block_size = 1024 << s_log_block_size`,
   `sectors_per_block = block_size / 512`, `inode_size` (128 для rev 0,
   `s_inode_size` для rev 1), `groups_count`.
4. Читается таблица дескрипторов групп (BGDT): блок 2 при block_size 1024,
   иначе блок 1. Лимит `bgdt_size_bytes <= 512` (до 16 групп). Контрольная
   проверка `bg_inode_bitmap != 0` отсеивает случай, когда контроллер вернул
   нули.
5. Том помечается `mounted = 1`.

`ext2_unmount()` запрещает размонтирование, пока у тома есть открытые файлы
(`FS_ACCESS_DENIED`).

## Иноды и адресация данных

Инод читается целиком (128 байт) в `ext2_file_t` при `open()` — дальше
операции идут по памяти без повторного чтения метаданных:

```
ext2_file_t { used, vol, inode_no, file_size, position, attributes, inode }
```

Трансляция `file_block_index → phys_block` в `ext2_read()`:

```
 i_block[0..11]                       прямые блоки
 i_block[12]  → таблица[256]          single indirect
 i_block[13]  → таблица → таблица     double indirect
 i_block[14]  → таблица → таблица → таблица   triple indirect
 phys_block == 0  → разреженный блок: буфер заполняется нулями (sparse)
```

`ptrs = block_size / 4` (256 указателей при блоке 1024). Чтение идёт блоками
через временные буферы `block_buf` и `indirect_buf`, выделенные из kernel heap
на время вызова. Чтение за концом файла обрезается, возвращает число байт.

## Поддержанные операции (fs_driver_api_t)

| Функция | Что делает |
|---|---|
| `mount / unmount / get_volume` | монтирование томов (см. выше) |
| `open / close / read / write` | файлы; до `EXT2_MAX_OPEN_FILES = 16` дескрипторов |
| `seek / tell` | позиция чтения/записи |
| `lookup / readdir` | поиск в каталоге и последовательное чтение dirents |
| `touch` | создание пустого файла |
| `mkdir / rmdir` | создание/удаление каталога (с проверкой пустоты) |
| `unlink / rename` | удаление и переименование (уменьшение links_count) |
| `stat` | размер, атрибуты, время create/modify |
| `statfs` | block_size, total/free blocks, total/free inodes |
| `relink_drive_id` | ядро корректирует привязку тома при смене id диска |

Дисковый слой `ext2_disk.c` дополняет это:

- `ext2_alloc_block / ext2_alloc_inode` — поиск свободного бита в bitmap
  группы, установка бита, обновление счётчиков в BGDT и суперблоке;
- `ext2_free_single_block / ext2_free_block_tree(depth)` — рекурсивное
  освобождение дерева косвенных блоков (depth 0..3);
- `ext2_truncate_inode` — усечение файла с освобождением всех блоков;
- `ext2_write_inode / ext2_write_block` — запись метаданных на место.

## Доступ к диску: block-API или прямая ATA

Каждая дисковая операция обёрнута в `KSL_ACQUIRE/KSL_RELEASE(schedule_lock_ptr)`:

- если `block_device_driver_enabled` — идёт через `block->read/write`
  (ATA-диск или USB-MSC, неважно);
- иначе (block-драйвер не загрузился) — fallback `ext2_disk_read_sectors()`:
  собственная PIO-реализация чтения на портах 0x1F0 с теми же задержками
  400 нс и дренажом залипшего DRQ.

Блокировка `schedule_lock` не даёт планировщику переключить задачу в середине
PIO-цикла. Важное правило (см. комментарий в `drv_format.h`): драйвер становится
**владельцем** блокировки только если она была свободна (`KSL_ACQUIRE`) и
снимает её только если владелец — он сам (`KSL_RELEASE`). Ядро держит
`kernel_schedule_lock` на всё время `systemcall_spawn_process`, и драйвер не
имеет права безусловно обнулять чужой флаг.

## Отладка

- `api->__ext2_dbg_hit_lba` — диагностика монтирования: при провале сканирования
  код упаковывается как `0xAA000000 | (r&0xFF)<<16 | magic_on_disk`.
- Точки отказа монтирования печатаются на загрузочный экран через
  `load_screen_add_string` (см. `use_drivers.c`).

## Сборка

```
gcc  -m32 -march=i386 -ffreestanding -fno-common -fno-stack-protector
     -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie
     -c drivers/ext2_driver/ext2_driver.c -o bin/ext2_driver.o
     -c drivers/ext2_driver/ext2_disk.c   -o bin/ext2_disk.o
nasm -f elf32 drivers/ext2_driver/ext2_start.asm -o bin/ext2_start.o
ld -m elf_i386 --no-warn-rwx-segments --emit-relocs -T drivers/userspace_driver.ld \
   bin/ext2_start.o bin/ext2_driver.o bin/ext2_disk.o bin/drv_ports_io.o bin/dlib.o \
   -o bin/ext2_driver.elf
```

Загружается пятым (`boot_info->modules[4]`), регистрируется как
`fs_apis[1]` c типом `FS_EXT2`; ядро внедряет `schedule_lock_ptr`,
`malloc_ptr`, `free_ptr`, `floppy_irq_fired_ptr` и `block`.

# arheoskernel