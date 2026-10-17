# Драйвер фреймбуфера для самодельной ОС

Драйвер фреймбуфера предоставляет базовые графические примитивы для ядра:

- отрисовка отдельных пикселей,

- заливка прямоугольников,

- вывод символов и строк,

- отображение чисел в шестнадцатеричном формате.

Используется как минимальный графический интерфейс для отладки и загрузки приложений.


## Функционал подробнее

- Поддержка BPP 16 (RGB565), 24 (RGB888), 32 (RGBA8888) с автоматическим преобразованием цвета.

- Шрифт фиксированного размера 8×8, встроенный в массив font_data_driver (CP866).

- Функции рисования:

- fb_put_pixel(x, y, color) – установка пикселя.

- fb_fill_rect(x, y, w, h, color) – закраска прямоугольника.

- fb_clear(color) – очистка экрана.

- font_draw_char(x, y, c, color) – вывод одного символа.

- font_draw_string(x, y, str, color) – вывод строки.

- font_draw_hex(dword, x, y) – вывод 32‑битного числа в шестнадцатеричном виде.

- Автоматическая адаптация к физическим параметрам фреймбуфера (width, height, pitch, BPP), передаваемым через boot_info.


Входным параметром при запуске драйвера используются поля "framebuffer" структуры boot_info описанная здесь: /include-kernel/bootinfo.h


```
typedef struct boot_info
{
    uint32_t magic;
    uint32_t version;
    uint32_t size;

    // framebuffer
    uint32_t framebuffer;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;

    // memory map
    uint32_t e820_addr;
    uint32_t e820_count;

    // filesystem
    filesystem_info_t *fs;

    // modules
    uint32_t module_count;
    module_info_t *modules;

    // strings
    char *cmdline;
    char *loader_name;
} boot_info_t;

```


Драйвер автоматически определяет bytes_pp и переключает режим записи: 16, 24 или 32 бита.



## Функции

# Рисование

```
void fb_put_pixel(int x, int y, uint32_t color);
```

Устанавливает пиксель с проверкой границ и преобразованием цвета.


```
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
```

Закрашивает прямоугольник.


```
void fb_clear(uint32_t color);
```

Очищает экран указанным цветом.


# Вывод шрифта

```
void font_draw_char(uint32_t x, uint32_t y, unsigned char c, uint32_t color);
```

Рисует один символ (8×8). Использует встроенный шрифт font_data_driver[c].


```
void font_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color);
```

Выводит строку последовательно (пробел = 1 символ).
Шестнадцатеричный вывод (отладка)

```
void font_draw_hex(uint32_t dword, int x, int y);
```

Выводит 8 шестнадцатеричных цифр (например, DEADBEEF).

```
void font_draw_hex_word(uint16_t word, int x, int y);   // 4 цифры
void draw_hex_byte(unsigned char byte, int x, int y);   // 2 цифры
```

Используются внутри для посимвольного вывода.


# Главная точка входа

```
void fb_driver_main(struct boot_info *boot);
```

Вызывается загрузчиком после инициализации.

Обычно выполняет:

- fb_init(boot) – настройка фреймбуфера.



## Процесс загрузки

Драйвер поставляется как позиционно‑независимый ELF-образ (скомпилированный с -fno-pic -fno-pie).
Ядро загружает его через загрузчик ELF (load_driver_elf), который:

Копирует секции .text, .rodata, .data и обнуляет .bss по адресу DRIVER_LOAD_ADDR (0x500000).

Обрабатывает все релокации (.rel.text, .rel.rodata, .rel.data) для типов R_386_32 и R_386_PC32.

Находит символ fb_driver_main и возвращает его адрес.

Ядро вызывает эту функцию, передавая boot_info.


## Сборка драйвера

Используются следующие флаги компиляции (GCC/clang):

```
CFLAGS = -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables
```

Линковка:
```
LD = ld -m elf_i386 --emit-relocs
```

Ассемблерный файл fb_start.asm (NASM) подготавливает заголовок драйвера DRV1 и вызывает fb_driver_main.

После линковки получается ELF‑файл fb_driver.elf, который укладывается на образ диска и дискеты после ядра и загружается впоследствии загрузчиком stage2.
После загрузки драйвера его адрес и размер добавляется в структуру bootinfo, которая передается ядру.

```
xxd -i fb_driver.elf > include-kernel/fb_driver_elf.h
```

В ядре используется load_driver_elf() из driver_elf.c для динамической загрузки.



# arheoskernel
