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
//
//Ключ -set переключает режим активного драйвера на ходу:
//  graphinfo -set 3               по индексу в списке режимов
//  graphinfo -set 800x600x16      по строке "ширина x высота x bpp"
//  graphinfo -set 800x600         по разрешению, bpp не важен
//После успешной смены ядро пересчитывает геометрию консолей и чистит экран

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

//сравнение строк без libc: u_lib с strcmp не линкуется в graphinfo.elf
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (*a == *b);
}

//строка целиком состоит из десятичных цифр и непуста
static int all_digits(const char *s)
{
    if (!s || !*s) return 0;
    while (*s)
    {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

//парсинг беззнакового десятичного числа, s продвигается за цифры.
//возвращает 0 при успехе, -1 если цифр на позиции не оказалось
static int parse_uint(const char **s, uint32_t *out)
{
    uint32_t v = 0;
    const char *p = *s;
    if (!p || *p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9')
    {
        v = v * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *s = p;
    *out = v;
    return 0;
}

//разбор строки режима "ШxВxГПП" или "ШxВ" (например "800x600x16").
//have_bpp = 0, если глубина цвета в строке не указана
static int parse_wxh(const char *s, uint32_t *w, uint32_t *h,
                     uint32_t *bpp, int *have_bpp)
{
    uint32_t v;
    if (parse_uint(&s, &v) != 0 || *s != 'x') return -1;
    *w = v;
    s++;
    if (parse_uint(&s, &v) != 0) return -1;
    *h = v;
    if (*s == '\0')
    {
        *bpp = 0;
        *have_bpp = 0;
        return 0;
    }
    if (*s != 'x') return -1;
    s++;
    if (parse_uint(&s, &v) != 0 || *s != '\0') return -1;
    *bpp = v;
    *have_bpp = 1;
    return 0;
}

//глубина цвета к каноническому виду: 15 и 16 бит — один и тот же
//класс режимов (см. приведение 15 -> 16 в fb_driver.c)
static uint32_t norm_bpp(uint32_t b)
{
    return (b == 15) ? 16 : b;
}

//поиск индекса режима по геометрии. При заданном bpp — только точное
//совпадение (с нормализацией 15/16); без bpp — первое совпадение
//по разрешению. Возвращает 0 и индекс через out, -1 если не нашли
static int find_mode_by_geom(uint32_t w, uint32_t h, uint32_t bpp, int have_bpp,
                             uint32_t mode_count, int32_t *out)
{
    for (uint32_t i = 0; i < mode_count; i++)
    {
        u_gr_mode_info_t m;
        if (u_gr_mode(i, &m) != 0) continue;
        if (m.xres != w || m.yres != h) continue;
        if (!have_bpp || norm_bpp(m.bpp) == norm_bpp(bpp))
        {
            *out = (int32_t)i;
            return 0;
        }
    }
    return -1;
}

//текстовое описание кода ошибки SYS_GRAPH_INFO (отрицательные GR_ERR_*)
static const char *gr_err_text(int rc)
{
    switch (rc)
    {
    case -1:
        return "generic error";
    case -2:
        return "no active graphics driver";
    case -3:
        return "driver cannot change mode";
    case -4:
        return "bad argument";
    case -7:
        return "io error during modeset";
    case -9:
        return "no such mode index";
    default:
        return "unknown error";
    }
}

static void print_usage(void)
{
    printf("usage: graphinfo [-set N | -set WxHxBPP | -set WxH]\n");
    printf("  graphinfo                  driver card, current mode, mode list\n");
    printf("  graphinfo -set 3           switch to mode by index\n");
    printf("  graphinfo -set 800x600x16  switch by geometry string\n");
    printf("  graphinfo -set 800x600     switch by resolution, any bpp\n");
}

//обработка "graphinfo -set СПЕЦ": смена режима активного драйвера.
//СПЕЦ — десятичный индекс режима или строка геометрии.
//Возвращает 0 при успехе, 1 при ошибке (сообщение уже выведено)
static int cmd_set_mode(const char *spec)
{
    u_gr_driver_info_t drv;
    if (u_gr_driver_info(&drv) != 0)
    {
        printf("graphinfo: no active graphics driver\n");
        return 1;
    }
    if (drv.mode_count == 0)
    {
        printf("graphinfo: driver '%s' has no modes\n", drv.name);
        return 1;
    }

    //индекс целевого режима: напрямую или через поиск по геометрии
    int32_t index = -1;
    if (all_digits(spec))
    {
        uint32_t v = 0;
        const char *p = spec;
        parse_uint(&p, &v);
        if (v >= drv.mode_count)
        {
            printf("graphinfo: mode index %d out of range, valid 0..%d\n",
                   (int)v, (int)drv.mode_count - 1);
            return 1;
        }
        index = (int32_t)v;
    }
    else
    {
        uint32_t w, h, bpp;
        int have_bpp;
        if (parse_wxh(spec, &w, &h, &bpp, &have_bpp) != 0)
        {
            printf("graphinfo: bad mode spec '%s'\n", spec);
            printf("expected: index | WxHxBPP | WxH\n");
            return 1;
        }
        if (find_mode_by_geom(w, h, bpp, have_bpp, drv.mode_count, &index) != 0)
        {
            printf("graphinfo: no mode '%s' in driver list\n", spec);
            return 1;
        }
    }

    int rc = u_gr_set_mode((uint32_t)index);
    if (rc != 0)
    {
        printf("graphinfo: mode set failed: %s\n", gr_err_text(rc));
        return 1;
    }

    //подтверждение: новый текущий режим после переключения
    printf("mode set: [");
    print_num((uint32_t)index, 3);
    printf("] ");
    u_gr_mode_info_t cur;
    if (u_gr_current_mode(&cur) == 0)
    {
        printf("%s, pitch %d, lfb 0x", cur.name, cur.pitch);
        print_hex8(cur.lfb);
        printf("\n");
    }
    else
    {
        printf("(current mode unavailable)\n");
    }
    return 0;
}

int main(int argc, char *argv[])
{
    //разбор аргументов: без ключей — полный отчет, -set — смена режима
    for (int i = 1; i < argc; i++)
    {
        if (streq(argv[i], "-set"))
        {
            if (i + 1 >= argc)
            {
                printf("graphinfo: -set requires an argument\n");
                print_usage();
                return 1;
            }
            if (cmd_set_mode(argv[i + 1]) != 0) return 1;
            i++;
        }
        else
        {
            printf("graphinfo: unknown option '%s'\n", argv[i]);
            print_usage();
            return 1;
        }
    }
    if (argc > 1) return 0;

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
