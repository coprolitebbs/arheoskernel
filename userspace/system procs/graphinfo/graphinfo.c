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

//graphinfo — отчет о видеодрайвере и видеорежимах.
//Через SYS_GRAPH_INFO получает карточку активного драйвера графического
//слоя, его текущий режим, fix-информацию и полный список доступных
//разрешений. Для fbdrv (fallback поверх штатного fb_driver) это один
//boot-режим из bootinfo, для vesa_fb — вся VBE-таблица, собранная
//загрузчиком. Список режимов соответствует тому, на что драйвер может
//переключиться (для vesa — через set_mode в gr_ops_t)

#include "../../include/ustd.h"
#include "../../include/ugraph.h"

//Десятичное число с выравниванием вправо в поле заданной ширины
static void print_num(uint32_t v, int width)
{
    char tmp[11];
    char out[12];
    int i = 0;
    int j = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0)
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (j < width - i) out[j++] = ' ';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
    printf("%s", out);
}

//32-битный адрес в hex фиксированной ширины 8
static void print_hex8(uint32_t v)
{
    const char *hx = "0123456789abcdef";
    char b[9];
    for (int i = 0; i < 8; i++) b[i] = hx[(v >> ((7 - i) * 4)) & 0xF];
    b[8] = '\0';
    printf("%s", b);
}

//Строка с дополнением пробелами до заданной ширины (длинные обрезаются)
static void print_str_pad(const char *s, int width)
{
    char b[17];
    int n = 0;
    while (s && s[n] && n < width && n < 16)
    {
        b[n] = s[n];
        n++;
    }
    while (n < width)
    {
        b[n] = ' ';
        n++;
    }
    b[n] = '\0';
    printf("%s", b);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    u_gr_driver_info_t drv;
    if (u_gr_driver_info(&drv) != 0)
    {
        printf("graphinfo: no active graphics driver\n");
        return 1;
    }

    printf("driver  : %s\n", drv.name);
    printf("          %s\n", drv.longname);
    printf("priority: %d, registry: %d, abi: %d\n",
           drv.priority, drv.driver_count, drv.abi_version);
    printf("modes   : %d, current index: %d\n", drv.mode_count, drv.current_mode);

    //Текущий режим: геометрия, формат пикселя, адрес LFB
    u_gr_mode_info_t cur;
    if (u_gr_current_mode(&cur) == 0)
    {
        printf("\ncurrent mode:\n");
        printf("  name %s, res %dx%d (virtual %dx%d)\n",
               cur.name, cur.xres, cur.yres, cur.xres_virtual, cur.yres_virtual);
        printf("  bpp %d, pitch %d, lfb 0x", cur.bpp, cur.pitch);
        print_hex8(cur.lfb);
        printf("\n");
        if (cur.red.length)    printf("  red   len %d off %d\n", cur.red.length, cur.red.offset);
        if (cur.green.length)  printf("  green len %d off %d\n", cur.green.length, cur.green.offset);
        if (cur.blue.length)   printf("  blue  len %d off %d\n", cur.blue.length, cur.blue.offset);
        if (cur.transp.length) printf("  alpha len %d off %d\n", cur.transp.length, cur.transp.offset);
        printf("  flags: %s\n", (cur.flags & U_GR_MODE_LFB) ? "linear framebuffer" : "banked");
    }

    //Fix-информация: неизменяемые свойства драйвера/видеопамяти
    u_gr_fix_info_t fix;
    if (u_gr_fix_info(&fix) == 0)
    {
        printf("\nfix info:\n");
        printf("  id %s, type %s, visual %s\n",
               fix.id, u_gr_type_name(fix.type), u_gr_visual_name(fix.visual));
        printf("  smem 0x");
        print_hex8(fix.smem_start);
        printf(", %d bytes, line_length %d\n", fix.smem_len, fix.line_length);
        if (fix.mmio_len)
        {
            printf("  mmio 0x");
            print_hex8(fix.mmio_start);
            printf(", %d bytes\n", fix.mmio_len);
        }
        else
        {
            printf("  mmio none, accel %s\n", fix.accel ? "present" : "none");
        }
    }

    //Полный список режимов активного драйвера
    printf("\n");
    printf("  [idx] mode        bpp  pitch   lfb        flags\n");
    for (uint32_t i = 0; i < drv.mode_count; i++)
    {
        u_gr_mode_info_t m;
        if (u_gr_mode(i, &m) != 0) continue;
        printf("  [");
        print_num(i, 3);
        printf("] ");
        print_str_pad(m.name, 12);
        print_num(m.bpp, 3);
        printf("  ");
        print_num(m.pitch, 5);
        printf("  0x");
        print_hex8(m.lfb);
        printf("  %s", (m.flags & U_GR_MODE_LFB) ? "LFB" : "BANK");
        if (i == drv.current_mode) printf("  <current>");
        printf("\n");
    }

    return 0;
}
