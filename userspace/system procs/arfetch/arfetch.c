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

#include "../../include/ustd.h"
#include "../../include/umemory.h"
#include "../../include/udevices.h"
#include "../../include/syscall.h"
#include "../../include/uimage.h"
#include "../../../include-kernel/lib.h"
#include <stdint.h>

//============ CPUID ============

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf)
    );
}

static void detect_cpu(char *vendor, char *brand)
{
    uint32_t a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    vendor[0]  = (char)((b)       & 0xFF);
    vendor[1]  = (char)((b >> 8)  & 0xFF);
    vendor[2]  = (char)((b >> 16) & 0xFF);
    vendor[3]  = (char)((b >> 24) & 0xFF);
    vendor[4]  = (char)((d)       & 0xFF);
    vendor[5]  = (char)((d >> 8)  & 0xFF);
    vendor[6]  = (char)((d >> 16) & 0xFF);
    vendor[7]  = (char)((d >> 24) & 0xFF);
    vendor[8]  = (char)((c)       & 0xFF);
    vendor[9]  = (char)((c >> 8)  & 0xFF);
    vendor[10] = (char)((c >> 16) & 0xFF);
    vendor[11] = (char)((c >> 24) & 0xFF);
    vendor[12] = '\0';

    int max_leaf = (int)a;
    brand[0] = '\0';
    if (max_leaf < 0) return;

    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) return;

    uint32_t *p = (uint32_t *)brand;
    cpuid(0x80000002, 0, &p[0], &p[1], &p[2], &p[3]);
    cpuid(0x80000003, 0, &p[4], &p[5], &p[6], &p[7]);
    cpuid(0x80000004, 0, &p[8], &p[9], &p[10], &p[11]);
    brand[48] = '\0';

    int start = 0;
    while (brand[start] == ' ') start++;
    if (start > 0)
    {
        int i = 0;
        while (brand[start + i])
        {
            brand[i] = brand[start + i];
            i++;
        }
        brand[i] = '\0';
    }
}

//============ Числа ============

static void u32_to_dec(uint32_t v, char *out)
{
    if (v == 0)
    {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    char tmp[12];
    int n = 0;
    while (v > 0)
    {
        tmp[n++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

//Собирает "NN%" — знак '%' кладём прямо в буфер, не через формат,
//иначе ustd.c-овский printf съест его как спецификатор
static void build_pct_string(uint32_t pct, char *out)
{
    u32_to_dec(pct, out);
    int i = 0;
    while (out[i]) i++;
    out[i++] = '%';
    out[i] = '\0';
}

//============ Art ============

//Пирамида
static void draw_pyramid(void)
{
    printf("\x1B[37m");
    printf("\x1B[3;5H      /\\     ");
    printf("\x1B[4;5H     /  \\    ");
    printf("\x1B[5;5H    / /\\ \\   ");
    printf("\x1B[6;5H   / /  \\ \\  ");
    printf("\x1B[7;5H  / /    \\ \\ ");
    printf("\x1B[8;5H / /______\\ \\");
    printf("\x1B[9;5H===============");
    printf("\x1B[0m");
}

//Анкх — лиловый
static void draw_ankh(void)
{
    printf("\x1B[36m");
    printf("\x1B[11;8H ___  ");
    printf("\x1B[12;8H/   \\ ");
    printf("\x1B[13;8H\\___/ ");
    printf("\x1B[14;8H  |   ");
    printf("\x1B[15;8H--+-- ");
    printf("\x1B[16;8H  |   ");
    printf("\x1B[17;8H  |   ");
    printf("\x1B[0m");
}

//Палитра ANSI — 8 блоков через background-цвета
//Блок 40 (чёрный) на чёрном фоне не виден — это ожидаемо, зато остальные 7 отлично показывают, что консоль жива
static void draw_palette(void)
{
    for (int i = 0; i < 8; i++)
    {
        printf("\x1B[%dm  \x1B[0m", 40 + i);
    }
}

//============ Main ============

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    uint32_t cols = 80, rows = 25;
    u_get_console_geometry(&cols, &rows, NULL, NULL);
    if (cols == 0) cols = 80;
    if (rows == 0) rows = 25;

    //Данные
    am_mem_info_t mem;
    get_mem_info(&mem);

    uint32_t pitch, bpp, width, height;
    get_fb_info(&pitch, &bpp, &width, &height);

    uint32_t ticks = get_ticks();
    int usb_count  = u_usb_count();
    int locale_idx = u_locale_get();

    char cpu_vendor[16] = {0};
    char cpu_brand[64]  = {0};
    detect_cpu(cpu_vendor, cpu_brand);
    const char *cpu_name = (cpu_brand[0]) ? cpu_brand : cpu_vendor;
    if (cpu_name[0] == '\0') cpu_name = "unknown";

    //Uptime
    uint32_t secs_total = ticks / 100;
    uint32_t secs  = secs_total % 60;
    uint32_t mins  = (secs_total / 60) % 60;
    uint32_t hours = secs_total / 3600;

    //Память — как в free: used / total MiB + процент загрузки
    uint32_t total_kb  = (mem.total_pages * 4096) / 1024;
    uint32_t used_kb   = (mem.used_pages  * 4096) / 1024;
    uint32_t total_mib = total_kb / 1024;
    uint32_t used_mib  = used_kb  / 1024;
    uint32_t load_pct  = total_kb ? (used_kb * 100) / total_kb : 0;

    //Форматирование
    char buf_h[12], buf_m[12], buf_s[12];
    char buf_total[12], buf_used[12];
    char buf_w[12], buf_hres[12];
    char buf_usb[12];
    char buf_pct[8];

    u32_to_dec(hours,      buf_h);
    u32_to_dec(mins,       buf_m);
    u32_to_dec(secs,       buf_s);
    u32_to_dec(total_mib,  buf_total);
    u32_to_dec(used_mib,   buf_used);
    u32_to_dec(width,      buf_w);
    u32_to_dec(height,     buf_hres);
    u32_to_dec((uint32_t)usb_count, buf_usb);
    build_pct_string(load_pct, buf_pct);

    const char *locale_name = "??";
    if (locale_idx == 0)      locale_name = "en";
    else if (locale_idx == 1) locale_name = "ru";

    clear_screen();

    //Компактный режим
    if (cols < 50 || rows < 20)
    {
        printf("\x1B[37m");
        printf("  ARHeOS - System Information\n");
        printf("  ==========================\n");
        printf("\x1B[0m");
        printf("\x1B[37mOS:         \x1B[36mARHeOS\x1B[0m\n");
        printf("\x1B[37mKernel:     \x1B[36m0.1.0\x1B[0m\n");
        printf("\x1B[37mUptime:     \x1B[36m%s h %s m %s s\x1B[0m\n",
               buf_h, buf_m, buf_s);
        printf("\x1B[37mShell:      \x1B[36msekhmet\x1B[0m\n");
        printf("\x1B[37mResolution: \x1B[36m%s x %s @ %d bpp\x1B[0m\n",
               buf_w, buf_hres, (int)bpp);
        printf("\x1B[37mRAM:        \x1B[36m%s / %s MiB (%s)\x1B[0m\n",
               buf_used, buf_total, buf_pct);
        printf("\x1B[37mCPU:        \x1B[36m%s\x1B[0m\n", cpu_name);
        printf("\x1B[37mUSB:        \x1B[36m%s device(s)\x1B[0m\n", buf_usb);
        printf("\x1B[37mLocale:     \x1B[36m%s\x1B[0m\n", locale_name);
        printf("\x1B[37mColors:     ");
        draw_palette();
        printf("\n");
        return 0;
    }

    //Широкий режим
    printf("\x1B[37m");
    printf("\x1B[2;23Hpharaoh@arheos");
    printf("\x1B[3;23H==================");
    printf("\x1B[0m");

    printf("\x1B[4;23H\x1B[37mOS:\x1B[36m          ARHeOS\x1B[0m");
    printf("\x1B[5;23H\x1B[37mKernel:\x1B[36m      0.1.0\x1B[0m");
    printf("\x1B[6;23H\x1B[37mUptime:\x1B[36m      %s h %s m %s s\x1B[0m",
           buf_h, buf_m, buf_s);
    printf("\x1B[7;23H\x1B[37mShell:\x1B[36m       sekhmet\x1B[0m");
    printf("\x1B[8;23H\x1B[37mResolution:\x1B[36m  %s x %s @ %d bpp\x1B[0m",
           buf_w, buf_hres, (int)bpp);
    printf("\x1B[9;23H\x1B[37mRAM:\x1B[36m         %s / %s MiB (%s)\x1B[0m",
           buf_used, buf_total, buf_pct);
    printf("\x1B[10;23H\x1B[37mCPU:\x1B[36m         %s\x1B[0m", cpu_name);
    printf("\x1B[11;23H\x1B[37mUSB:\x1B[36m         %s device(s)\x1B[0m", buf_usb);
    printf("\x1B[12;23H\x1B[37mLocale:\x1B[36m      %s\x1B[0m", locale_name);
    printf("\x1B[13;23H\x1B[37mColors:\x1B[0m      ");
    draw_palette();

    //Искусство
    draw_pyramid();
    draw_ankh();

    //Курсор в последнюю строку
    uint32_t cursor_row = (rows > 1) ? (rows - 1) : 1;
    printf("\x1B[%d;1H", cursor_row);

    return 0;
}
