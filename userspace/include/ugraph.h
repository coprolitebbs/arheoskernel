//Copyright (C) 2026 ARHeOS by Dmitry H
//
//This program is free software: you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//GNU General Public License for more details.
//
//You should have received a copy of the GNU General Public License
//along with this program. If not, see <https://www.gnu.org/licenses/>.

#ifndef UGRAPH_H
#define UGRAPH_H

#include <stdint.h>

//Пользовательские зеркала структур графического слоя
//(drivers/include_drivers/driver_graphic_layer.h).
//Используются одновременно ядром (systemcall_graph_info — побайтовый
//маршалинг через SYS_GRAPH_INFO) и userspace (программа graphinfo).
//Все стороны договариваются об одном layout: при расхождении полей
//сработают _Static_assert в syscalls.c

//флаги режима (зеркало GR_MODE_*)
#define U_GR_MODE_LFB      0x01
#define U_GR_MODE_DEFAULT  0x02

//тип организации видеопамяти (зеркало GR_TYPE_*)
#define U_GR_TYPE_PACKED_PIXELS 0
#define U_GR_TYPE_PLANES        1
#define U_GR_TYPE_VGA_PLANES    2

//визуальный класс (зеркало GR_VISUAL_*)
#define U_GR_VISUAL_MONO01      0
#define U_GR_VISUAL_MONO10      1
#define U_GR_VISUAL_PSEUDOCOLOR 2
#define U_GR_VISUAL_TRUECOLOR   3
#define U_GR_VISUAL_DIRECTCOLOR 4

//аппаратное ускорение (зеркало GR_ACCEL_*)
#define U_GR_ACCEL_NONE      0x0000
#define U_GR_ACCEL_FILLRECT  0x0001
#define U_GR_ACCEL_BLIT      0x0002
#define U_GR_ACCEL_COPYAREA  0x0004
#define U_GR_ACCEL_HLINE     0x0008
#define U_GR_ACCEL_VLINE     0x0010

typedef struct
{
    uint8_t offset;
    uint8_t length;
    uint8_t msb_right;
    uint8_t reserved;
} u_gr_bitfield_t;

//Зеркало gr_mode_info_t — 72 байта, layout обязан совпадать
typedef struct
{
    uint32_t index;
    char     name[16];                 //"640x480x32", "boot" и т.п.
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t bpp;
    u_gr_bitfield_t red, green, blue, transp;
    uint32_t pixclock_khz;
    uint32_t lfb;                      //адрес LFB (virt == phys)
    uint32_t pitch;                    //длина строки в байтах
    uint32_t flags;                    //U_GR_MODE_*
} u_gr_mode_info_t;

//Зеркало gr_fix_info_t — 56 байт, layout обязан совпадать
typedef struct
{
    char     id[16];                   //имя драйвера: "vesa", "fbdrv"
    uint32_t smem_start;
    uint32_t smem_len;
    uint32_t type;                     //U_GR_TYPE_*
    uint32_t visual;                   //U_GR_VISUAL_*
    uint32_t line_length;
    uint32_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;                    //U_GR_ACCEL_*
    uint32_t ypanstep;
    uint32_t ywrapstep;
} u_gr_fix_info_t;

//Карточка активного видеодрайвера (заполняется ядром из gr_driver_t)
typedef struct
{
    uint32_t abi_version;              //GR_LAYER_ABI_VERSION драйвера
    char     name[24];                 //короткое имя: "vesa", "fbdrv"
    char     longname[48];
    uint32_t priority;                 //у fbdrv-fallback = 0
    uint32_t mode_count;               //число режимов, видимых слоем
    uint32_t current_mode;             //индекс текущего режима в списке
    uint32_t driver_count;             //всего драйверов в реестре слоя
} u_gr_driver_info_t;

//================= обертки SYS_GRAPH_INFO (userspace) =================
//Все возвращают 0 при успехе, -1 при ошибке ядра

//Карточка активного видеодрайвера
int u_gr_driver_info(u_gr_driver_info_t *out);

//Режим по индексу (0 .. mode_count-1)
int u_gr_mode(uint32_t index, u_gr_mode_info_t *out);

//Текущий режим активного драйвера
int u_gr_current_mode(u_gr_mode_info_t *out);

//Fix-информация активного драйвера
int u_gr_fix_info(u_gr_fix_info_t *out);

//Смена режима активного драйвера по индексу (0 .. mode_count-1).
//Возвращает 0 при успехе, иначе отрицательный код GR_ERR_* ядра:
//-2 нет активного драйвера, -3 драйвер не умеет менять режим
//(fbdrv-fallback), -7 ошибка ввода-вывода при modeset, -9 нет такого
//индекса. После успешной смены ядро само пересчитывает геометрию консолей
//и очищает экран
int u_gr_set_mode(uint32_t index);

//Примитивы графического слоя из Ring-3 (SYS_GR_PRIM): заливка, линия
//(в том числе диагональные — на Geode это аппаратный векторный движок GP),
//копирование области внутри экрана и блит RAM->FB.
//params — массив int32[6], раскладка по op (см. GR_PRIM_* в syscall.h):
//  GR_PRIM_FILL_RECT: x, y, w, h, color
//  GR_PRIM_DRAW_LINE: x0, y0, x1, y1, color
//  GR_PRIM_COPY_AREA: sx, sy, w, h, dx, dy
//  GR_PRIM_BLIT:      x, y, w, h, указатель на буфер пикселей
//color — 0x00RRGGBB; буфер blit — пиксели в формате текущего режима
//(упакованные строки w*(bpp/8) байт, без padding).
//Возвращает 0 при успехе, иначе отрицательный код GR_ERR_* ядра
int u_gr_prim(uint32_t op, const int32_t *params);

//Утилиты: строковые имена полей fix-информации (без malloc)
const char *u_gr_visual_name(uint32_t visual);
const char *u_gr_type_name(uint32_t type);

#endif
