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

#ifndef DRIVER_GRAPHIC_LAYER_H
#define DRIVER_GRAPHIC_LAYER_H

/*
 * driver_graphic_layer.h — высокоуровневый слой абстракции графики
 *
 * Схема (по образцу Linux framebuffer API: fb_var_screeninfo /
 * fb_fix_screeninfo / fb_ops / fb_info, в упрощённом виде):
 *
 *     ядро / GUI / системные процессы
 *                 |
 *        gr_* функции слоя        (graphic_layer.c, Ring 0)
 *                 |
 *     gr_driver_t + gr_ops_t      <-- любой драйвер: VESA, Geode GX1/LX, fbdrv
 *                 |
 *               железо
 *
 * Заголовок общий для ядра и драйверов, чтобы описания структур совпадали
 * у обеих сторон. Нижний драйвер заполняет gr_driver_t и отдаёт его ядру
 * через description_driver_api_t (type = DESCR_GRAPHICS_DRIVER, см.
 * drv_format.h). Регистрацией, выбором активного драйвера и программными
 * fallback'ами ведает graphic_layer.c (компилируется в ядро).
 *
 * Функции gr_* объявлены здесь для общности, но драйверы их НЕ линкуют:
 * они вызываются только из кода ядра.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
//#include "../../include-kernel/vbe_rm.h"

//---------------- ABI ----------------
#define GR_LAYER_ABI_VERSION 1

//---------------- коды ошибок ----------------
#define GR_OK                0
#define GR_ERR_GENERIC     (-1)
#define GR_ERR_NODEV       (-2)   //нет активного драйвера
#define GR_ERR_UNSUPPORTED (-3)   //операция не поддерживается (заглушка)
#define GR_ERR_BADARG      (-4)   //некорректные аргументы/выход за экран
#define GR_ERR_BUSY        (-5)   //драйвер уже зарегистрирован
#define GR_ERR_NOMEM       (-6)
#define GR_ERR_IO          (-7)
#define GR_ERR_NOTFOUND    (-8)   //драйвер с таким именем не найден
#define GR_ERR_BADMODE     (-9)   //режима с таким индексом нет

//---------------- флаги режима (gr_mode_info_t.flags) ----------------
#define GR_MODE_LFB      0x01    //линейный фреймбуфер
#define GR_MODE_DEFAULT  0x02    //режим по умолчанию (метка драйвера)

//тип организации видеопамяти (gr_fix_info_t.type) — как в fb_fix_screeninfo
#define GR_TYPE_PACKED_PIXELS 0
#define GR_TYPE_PLANES        1
#define GR_TYPE_VGA_PLANES    2

//визуальный класс (gr_fix_info_t.visual)
#define GR_VISUAL_MONO01      0
#define GR_VISUAL_MONO10      1
#define GR_VISUAL_PSEUDOCOLOR 2   //8bpp с палитрой
#define GR_VISUAL_TRUECOLOR   3
#define GR_VISUAL_DIRECTCOLOR 4

//аппаратное ускорение (gr_fix_info_t.accel)
#define GR_ACCEL_NONE      0x0000
#define GR_ACCEL_FILLRECT  0x0001
#define GR_ACCEL_BLIT      0x0002
#define GR_ACCEL_COPYAREA  0x0004
#define GR_ACCEL_HLINE     0x0008
#define GR_ACCEL_VLINE     0x0010

//Область тримплина в младшей памяти. Адреса обязаны совпадать с
//vbe_rm_blob.asm и gdt.c (дескрипторы с базой 0x4000 на индексах 128/129)
//Зона 0x4000..0x4BFF не занята никем: BDA заканчивается на 0x4FF,
//bootinfo с 0x5000, stage2 с 0x7E00, pmm никогда не раздаёт память ниже 1 МБ
//Карта: код блоба 0x4000.., вход-параметры 0x4300, результаты 0x4320,
//служебные ячейки 0x4340..0x434f, метка целостности 0x4350, буфер VBE
//4F01 (1 КБ) 0x4400..0x47FF. Служебная зона вынесена за код блоба, чтобы
//runtime-записи блоба физически не могли затереть код возврата
#define VBE_RM_BLOB_ADDR 0x4000
#define VBE_RM_BLOB_MAX  0x400
#define VBE_RM_REQ_ADDR  0x4300
#define VBE_RM_RES_ADDR  0x4320
#define VBE_RM_INFO_ADDR 0x4400
#define VBE_RM_INFO_SIZE 0x400




//---------------- описание режима (аналог fb_var_screeninfo) --------
typedef struct gr_bitfield
{
    uint8_t offset;      //смещение поля от младшего бита пикселя
    uint8_t length;      //число битов поля (0 = поле не используется)
    uint8_t msb_right;   //как в fb_bitfield, для совместимости
    uint8_t reserved;
} gr_bitfield_t;

typedef struct gr_mode_info
{
    uint32_t index;                       //номер режима внутри драйвера
    char     name[16];                    //"640x480x32" и т.п.
    uint32_t xres, yres;                  //видимое разрешение
    uint32_t xres_virtual, yres_virtual;  //виртуальное разрешение (panning)
    uint32_t bpp;                         //бит на пиксель: 8/16/24/32
    gr_bitfield_t red, green, blue, transp;
    uint32_t pixclock_khz;                //пиксельные часы, кГц (0 = неизвестны)
    uint32_t lfb;                         //адрес LFB (в ARHeOS virt == phys)
    uint32_t pitch;                       //длина строки в байтах
    uint32_t flags;                       //GR_MODE_*
} gr_mode_info_t;

//---------------- неизменяемая часть (аналог fb_fix_screeninfo) ------
typedef struct gr_fix_info
{
    char     id[16];          // имя драйвера/карты: "geode_lx", "vesa"
    uint32_t smem_start;      // начало видеопамяти
    uint32_t smem_len;        // размер видеопамяти
    uint32_t type;            // GR_TYPE_*
    uint32_t visual;          // GR_VISUAL_*
    uint32_t line_length;     // длина строки в байтах
    uint32_t mmio_start;      // начало MMIO-регистров (0 = нет)
    uint32_t mmio_len;
    uint32_t accel;           // GR_ACCEL_*
    uint32_t ypanstep;        // шаг вертикального panning
    uint32_t ywrapstep;
} gr_fix_info_t;

//Набор регистров для вызова BIOS int 10h через тримплин.
//es на входе - СЕГМЕНТНЫЙ адрес буфера структуры (phys >> 4), 0 если не нужен
//на выходе - фактический ES после BIOS-вызова
typedef struct vbe_rm_regs
{
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t di;
    uint16_t si;
    uint16_t es;
} __attribute__((packed)) vbe_rm_regs_t;


//---------------- операции драйвера (аналог fb_ops) ------------------
//Все функции необязательны (NULL): слой подставит программную реализацию
//через LFB текущего режима или вернёт GR_ERR_UNSUPPORTED.
//Первый аргумент — priv из gr_driver_t
typedef struct gr_ops
{
    uint32_t version;   //= GR_LAYER_ABI_VERSION

    //перечисление и смена режимов
    int (*get_mode_count)(void *priv);
    int (*get_mode)(void *priv, uint32_t index, gr_mode_info_t *out);
    int (*set_mode)(void *priv, uint32_t index);
    int (*get_var)(void *priv, gr_mode_info_t *out);   //текущий режим
    int (*get_fix)(void *priv, gr_fix_info_t *out);

    //примитивы
    int (*put_pixel)(void *priv, int x, int y, uint32_t color);  //0x00RRGGBB
    int (*get_pixel)(void *priv, int x, int y, uint32_t *out);
    int (*fill_rect)(void *priv, int x, int y, int w, int h, uint32_t color);
    int (*blit)(void *priv, int x, int y, int w, int h, const void *src);
    int (*copy_area)(void *priv, int sx, int sy, int w, int h, int dx, int dy);

    //управление выводом
    int (*pan_display)(void *priv, uint32_t yoffset);
    int (*blank)(void *priv, int blank);   //1 = погасить, 0 = включить
    int (*wait_vsync)(void *priv);
    //rgb: count triplets R,G,B по одному байту (для 8bpp-палитры)
    int (*set_palette)(void *priv, uint32_t start, uint32_t count, const uint8_t *rgb);
} gr_ops_t;

//---------------- дескриптор драйвера (аналог fb_info) ----------------
typedef struct gr_driver
{
    uint32_t version;       // = GR_LAYER_ABI_VERSION
    char     name[24];      //короткое имя: "vesa", "geode_gx1", "fbdrv"
    char     longname[48];  //развёрнутое имя для логов
    uint32_t flags;         //зарезервировано, 0
    uint32_t priority;      //чем больше, тем вероятнее станет активным; fbdrv = 0

    //статический список режимов; если modes == NULL, драйвер обязан
    //реализовать get_mode_count/get_mode в ops
    const gr_mode_info_t *modes;
    uint32_t mode_count;

    const gr_ops_t *ops;
    void *priv;             //приватные данные драйвера

    uint32_t current_mode;  //индекс текущего режима (0 по умолчанию)

    // --- внедряемый ядром рантайм (заполняет graphic_layer.c при
    //регистрации; драйвер может ничего тут не писать) ---
    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);
    void  (*yield_ptr)(void);
    void  (*vmm_map_pages_ptr)(uint32_t phys, uint32_t size);
    void  (*vmm_map_pages_uncached_ptr)(uint32_t phys, uint32_t size);
    void  (*printf_ptr)(const char *fmt, ...);   /* kprintf ядра */

    //Вызов функций BIOS int 10h (VBE) через real-mode тримплин vbe_rm.c.
    //Нужно драйверам, умеющим менять видеорежим на ходу (vesa_fb).
    int (*vbe_rm_exec_ptr)(vbe_rm_regs_t *regs);

    void *native_driver_api;  //Адрес на структуру нативных функций драйвера (например, дополнительных) - это для совместимости с дефолтным драйвером фреймбуффера
} gr_driver_t;




#endif /* KERNEL_GRAPHIC_LAYER_H */
