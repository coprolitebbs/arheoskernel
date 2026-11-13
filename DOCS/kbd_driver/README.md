# Драйвер клавиатуры PS/2 (i8042)

Драйвер аппаратной клавиатуры IBM PC/AT (контроллер i8042). Разделение
обязанностей сделано так:

- **драйвер** — принимает сырые скан-коды, ведёт кольцевой буфер событий,
  управляет светодиодом CapsLock;
- **ядро** — вызывает драйвер из обработчика IRQ1 и из системного вызова
  `SYS_READ`, транслирует скан-коды в символы (`kbd_translate.c`,
  таблица с учётом раскладки и 0xE0-префиксов).

Исходники: `drivers/kbd_driver/` (`kbd_driver.c`, `kbd_start.asm`,
`include/kbd_driver.h`, `include/kbd_internal.h`).

## Схема работы

```
   Клавиатура (scan set 1)
        │  IRQ1
        v
   ┌─────────────── ЯДРО (isr.c) ─────────────────┐
   │  int 33 = irq1:                              │
   │   scancode = inb(0x60)                       │
   │   if (kbd_driver_api && irq_callback)        │
   │       kbd_driver_api->irq_callback(scancode) │
   └──────────────────────┬───────────────────────┘
                          v
   ┌──────────────── kbd_driver.c ─────────────────┐
   │  Кольцевой буфер kbd_event_buf[256]:          │
   │  kbd_event_t { scancode; type }               │
   │     type = 1 нажатие (Press)                  │
   │     type = 0 отпускание (Release)             │
   │  0x3A CapsLock → state  LED (0xED)           │
   │  0xE0-префикс пишется отдельным событием      │
   └──────────────────────┬────────────────────────┘
                          │  SYS_READ (stdin)
                          v
   ┌──────────────── ЯДРО (syscalls.c) ────────────┐
   │  if (kbd_driver_api->has_chars())             │
   │      packed = kbd_driver_api->get_char_blocked();
   │      ch = kbd_translate_event(packed);        │
   └───────────────────────────────────────────────┘
```

## Аппаратные порты

```
KBD_DATA_PORT    0x60    // буфер данных: скан-коды, команды i8042
KBD_STATUS_PORT  0x64    // статус: бит 0x02 = "входной буфер занят"
```

Перед записью в 0x60 драйвер ждёт очистки бита 0x02 (`kbd_wait_write`,
с таймаутом 50000 итераций).

## Обработка прерываний

`kbd_irq_callback(uint8_t scancode)` вызывается ядром из обработчика IRQ1
и должен быть максимально быстрым (никаких ожиданий и yield внутри).
Логика:

1. **Управление LED**: команда смены индикаторов выполняется в два шага
   прямо внутри коллбэка — драйвер послал 0xED (`led_command_state = 1`),
   контроллер ответил ACK 0xFA, драйвер отправляет байт маски
   (бит 0x04 = CapsLock), получает ACK (`led_command_state = 2`, готово).
2. **CapsLock** (0x3A): переключает `driver_caps_lock_state` и запускает
   обновление LED при следующем удобном вызове.
3. **0xE0-префикс**: расширенные клавиши (стрелки и т.п.). Важный нюанс:
   у самого префикса взведён старший бит (0xE0 & 0x80), но «отрезать» его
   как отпускание нельзя — поэтому чистый 0xE0 пишется в буфер отдельным
   событием (type = 1), а следующий скан-код идёт следом. Трансляция пары
   0xE0код выполняется ядром.
4. **Обычные клавиши**: событие упаковывается в буфер —
   скан-код отпускания (старший бит 0x80) превращается в пару
   `{scancode = code & 0x7F, type = 0}`; нажатие — `{code, 1}`.
   Переполнение буфера (head догоняет tail) молча отбрасывается.

## API (kbd_driver_api_t)

| Функция | Кто вызывает | Поведение |
|---|---|---|
| `irq_callback(scancode)` | ядро, IRQ1 | приём скан-кода, буферизация, LED |
| `get_char_blocked()` | ядро, `SYS_READ` | возвращает упакованное событие `scancode | (type<<8)` в EAX; если буфер пуст — крутит `yield_ptr()` (блокирует задачу, но не CPU); попутно догоняет LED-состояние |
| `has_chars()` | ядро, `SYS_READ`/консоль | неблокирующий аналог kbhit: 1/0 |
| `malloc_ptr / free_ptr / yield_ptr` | внедряются ядром | heap и асинхронный сон |

Возврат через EAX в упакованном виде позволяет ядру отличить нажатие от
отпускания: `kbd_translate_event()` в `kbd_translate.c` использует type для
фильтрации автоповтора и обработки расширенных клавиш.

## Интеграция с ядром

- Загружается **динамически** из `/adrv/` по конфигу
  `/config/boot/boot.cfg` (секции `[driver_1]...`, ключ `name`),
  через `load_additional_drivers()` — в отличие от обязательных драйверов
  из модулей загрузчика.
- После загрузки ядро сохраняет указатель `kbd_driver_api` и внедряет
  `yield_ptr`.
- Ядро, а не драйвер, решает судьбу событий: консоль Сехмет и `SYS_READ`
  берут события через `has_chars()`  `get_char_blocked()`, для опроса
  «нажато ли что-то» есть отдельный syscall, возвращающий `has_chars()`.

## Сборка

```
gcc  -m32 -march=i386 -ffreestanding -fno-common -fno-stack-protector
     -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie
     -c drivers/kbd_driver/kbd_driver.c -o bin/kbd_driver.o
nasm -f elf32 drivers/kbd_driver/kbd_start.asm -o bin/kbd_start.o
ld -m elf_i386 --no-warn-rwx-segments --emit-relocs -T drivers/userspace_driver.ld \
   bin/kbd_start.o bin/kbd_driver.o bin/drv_ports_io.o bin/dlib.o \
   -o bin/kbd_driver.elf
```

Готовый `kbd_driver.elf` кладётся на диск в каталог `/adrv/`
(имя указывается в `boot.cfg`).

# arheoskernel