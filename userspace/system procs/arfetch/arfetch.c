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

// ============ CPUID ============

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf)
    );
}

static void detect_cpu(char *vendor, char *brand) {
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
    if (start > 0) {
        int i = 0;
        while (brand[start + i]) { brand[i] = brand[start + i]; i++; }
        brand[i] = '\0';
    }
}

// ============ Числа ============

static void u32_to_dec(uint32_t v, char *out) {
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12]; int n = 0;
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

// Собирает "NN%" — знак '%' кладём прямо в буфер, не через формат,
// иначе ustd.c-овский printf съест его как спецификатор.
static void build_pct_string(uint32_t pct, char *out) {
    u32_to_dec(pct, out);
    int i = 0;
    while (out[i]) i++;
    out[i++] = '%';
    out[i] = '\0';
}

// ============ Art ============
//
// Эмблемы рисуются построчно, каждая строка своим цветом VGA-палитры
// консоли. Позиционирование — \x1B[r;cH.
// Три варианта:
//   arfetch        — кошка Бастет (по умолчанию, маскот ArheOS)
//   arfetch eye    — Око Гора (ваджет)
//   arfetch ankh   — анкх
//
// Бастет нарисована блоками псевдографики █ ▀ ▄ (kconsole понимает их):
// половинные блоки дают двойное вертикальное разрешение — каждый текстовый
// ряд несёт два «пикселя» 8x8. Вся фигура — белый (37), чёрные детали
// (глаза, нос, бороздки воротника и лап) — это фон, просвечивающий
// сквозь незакрашенные половины. Ширина 28 колонок, 18 рядов.
// Око и анкх — чистый ASCII, ширина <= 32, оставлены как варианты.

#define LOGO_ROW 2   // верхняя строка логотипа (ANSI-нумерация с 1)
#define LOGO_COL 1   // левая колонка логотипа
#define INFO_COL 34  // колонка блока информации

// ---- Око Гора (ваджет): бровь и веки золото, глаз серебро, метки бирюза ----
static const char *const EYE_LINES[] = {
    "         ##############         ",
    "     ####              ####     ",
    "   ##       ########       ##   ",
    "        ####        ####        ",
    "     ###      ######      ###   ",
    "   ##       ########       ##   ",
    "   #       ###    ###       #   ",
    "   #       ###    ###       #   ",
    "    #       ########       #    ",
    "     ###      ######      ###   ",
    "     ## ####        ####        ",
    "     ##     ########            ",
    "     ##           ##########    ",
    "     ##                    ##   ",
    "     ##               ######    ",
    "     ####            ##         ",
    "                      ##        ",
};

static const unsigned char EYE_COLORS[] = {
    33, 33, 33, 33, 37, 37, 37, 37, 37, 33, 36, 36, 36, 36, 36, 36, 36,
};

// ---- Анкх: золото, 18 строк ----
static const char *const ANKH_LINES[] = {
    "        .######.        ",
    "      ###      ###      ",
    "     ##          ##     ",
    "    ##            ##    ",
    "    ##            ##    ",
    "    ##            ##    ",
    "    ##            ##    ",
    "     ##          ##     ",
    "      ###      ###      ",
    "       ##      ##       ",
    "########################",
    "########################",
    "          ###           ",
    "          ###           ",
    "          ###           ",
    "          ###           ",
    "          ###           ",
    "          ###           ",
};

static const unsigned char ANKH_COLORS[] = {
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
};

// ---- Бастет (маскот ArheOS): блоки █▀▄, белый, 18 рядов x 28 колонок ----
// Каждый текстовый ряд = 2 пиксельных ряда (полублоки удваивают разрешение):
// ▀ — белый верх/чёрный низ, ▄ — чёрный верх/белый низ, █ — оба белые.
static const char *const CAT_LINES[] = {
    "       ▄███      ███▄       ",
    "      ▄█████    █████▄      ",
    "     ▄███████  ███████▄     ",
    "    ▄██████████████████▄    ",
    "   ▄████████████████████▄   ",
    "  ████████████████████████  ",
    " █████▀▀▀▀████████▀▀▀▀█████ ",
    " ███▄▄▄▄   ▀████▀   ▄▄▄▄███ ",
    " ▀████████████████████████▀ ",
    "   ▀████████▄  ▄████████▀   ",
    "     ▀▀██████▄▄██████▀▀     ",
    "       ▄▄██████████▄▄       ",
    "   ▄▄▀▀▀▀▀▀██████▀▀▀▀▀▀▄▄   ",
    "  ▀██████████████████████▀  ",
    "    ████████████████████    ",
    "    █████ ██ ██ ██ █████    ",
    "   ▄█████▄██▄██▄██▄█████▄   ",
    " ▄████████████████████████▄ ",
};

static const unsigned char CAT_COLORS[] = {
    37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37,
};

// Печать логотипа построчно с построчным цветом.
static void draw_art(const char *const *lines, const unsigned char *colors, int n) {
    for (int i = 0; i < n; i++) {
        printf("\x1B[%d;%dH\x1B[%dm%s",
               LOGO_ROW + i, LOGO_COL, (int)colors[i], lines[i]);
    }
    printf("\x1B[0m");
}

// ============ Утилиты строк ============

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (*a == *b);
}

static char lowc(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Причёсывает название CPU, чтобы строка гарантированно влезла в 80 колонок:
// выбрасывает короткие скобки "(R)"/"(TM)"/"(C)", слова processor/CPU,
// схлопывает пробелы; если всё равно длинно — укорачивает по границе
// слова и ставит многоточие. cap — максимум символов в результате.
static void tidy_cpu(const char *src, char *dst, int cap) {
    int i = 0, j = 0;
    while (src[i] != '\0' && j < cap) {
        if (src[i] == '(') {
            int k = i + 1;
            while (src[k] != '\0' && src[k] != ')' && (k - i) < 6) k++;
            if (src[k] == ')') { i = k + 1; continue; }
        }
        if (src[i] == ' ') {
            if (j > 0 && dst[j - 1] != ' ') dst[j++] = ' ';
            i++;
            continue;
        }
        int s = i;
        while (src[i] != '\0' && src[i] != ' ' && src[i] != '(') i++;
        int wlen = i - s;
        if (wlen == 9 || wlen == 3) {
            const char *ref = (wlen == 9) ? "processor" : "cpu";
            int match = 1;
            for (int t = 0; t < wlen; t++) {
                if (lowc(src[s + t]) != ref[t]) { match = 0; break; }
            }
            if (match) continue;
        }
        for (int t = 0; t < wlen && j < cap; t++) dst[j++] = src[s + t];
        if (j < cap && src[i] != '\0') dst[j++] = ' ';
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = '\0';
    if (j > cap - 4) {
        int cut = cap - 4;
        while (cut > 0 && dst[cut] != ' ') cut--;
        dst[cut]     = '.';
        dst[cut + 1] = '.';
        dst[cut + 2] = '.';
        dst[cut + 3] = '\0';
    }
}

// ============ Палитра ANSI ============

// Палитра ANSI — 8 блоков через background-цвета.
// Блок 40 (чёрный) на чёрном фоне не виден — это ожидаемо,
// зато остальные 7 отлично показывают, что консоль жива.
static void draw_palette(void) {
    for (int i = 0; i < 8; i++) {
        printf("\x1B[%dm  \x1B[0m", 40 + i);
    }
}

// ============ Main ============

int main(int argc, char *argv[]) {

    uint32_t cols = 80, rows = 25;
    u_get_console_geometry(&cols, &rows, NULL, NULL);
    if (cols == 0) cols = 80;
    if (rows == 0) rows = 25;

    // ---- Данные ----
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

    // Укороченное название CPU — гарантированно влезает в строку
    char cpu_tidy[48];
    tidy_cpu(cpu_name, cpu_tidy, 30);

    // Uptime
    uint32_t secs_total = ticks / 100;
    uint32_t secs  = secs_total % 60;
    uint32_t mins  = (secs_total / 60) % 60;
    uint32_t hours = secs_total / 3600;

    // Память — как в free: used / total MiB + процент загрузки
    uint32_t total_kb  = (mem.total_pages * 4096) / 1024;
    uint32_t used_kb   = (mem.used_pages  * 4096) / 1024;
    uint32_t total_mib = total_kb / 1024;
    uint32_t used_mib  = used_kb  / 1024;
    uint32_t load_pct  = total_kb ? (used_kb * 100) / total_kb : 0;

    // ---- Форматирование ----
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

    // ---- Компактный режим ----
    if (cols < 78 || rows < 20) {
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
        printf("\x1B[37mCPU:        \x1B[36m%s\x1B[0m\n", cpu_tidy);
        printf("\x1B[37mUSB:        \x1B[36m%s device(s)\x1B[0m\n", buf_usb);
        printf("\x1B[37mLocale:     \x1B[36m%s\x1B[0m\n", locale_name);
        printf("\x1B[37mColors:     ");
        draw_palette();
        printf("\n");
        return 0;
    }

    // ---- Широкий режим ----
    printf("\x1B[37m");
    printf("\x1B[2;%dHpharaoh@arheos", INFO_COL);
    printf("\x1B[3;%dH==================", INFO_COL);
    printf("\x1B[0m");

    printf("\x1B[4;%dH\x1B[37mOS:\x1B[36m          ARHeOS\x1B[0m", INFO_COL);
    printf("\x1B[5;%dH\x1B[37mKernel:\x1B[36m      0.1.0\x1B[0m", INFO_COL);
    printf("\x1B[6;%dH\x1B[37mUptime:\x1B[36m      %s h %s m %s s\x1B[0m", INFO_COL,
           buf_h, buf_m, buf_s);
    printf("\x1B[7;%dH\x1B[37mShell:\x1B[36m       sekhmet\x1B[0m", INFO_COL);
    printf("\x1B[8;%dH\x1B[37mResolution:\x1B[36m  %s x %s @ %d bpp\x1B[0m", INFO_COL,
           buf_w, buf_hres, (int)bpp);
    printf("\x1B[9;%dH\x1B[37mRAM:\x1B[36m         %s / %s MiB (%s)\x1B[0m", INFO_COL,
           buf_used, buf_total, buf_pct);
    printf("\x1B[10;%dH\x1B[37mCPU:\x1B[36m         %s\x1B[0m", INFO_COL, cpu_tidy);
    printf("\x1B[11;%dH\x1B[37mUSB:\x1B[36m         %s device(s)\x1B[0m", INFO_COL, buf_usb);
    printf("\x1B[12;%dH\x1B[37mLocale:\x1B[36m      %s\x1B[0m", INFO_COL, locale_name);
    printf("\x1B[13;%dH\x1B[37mColors:\x1B[0m      ", INFO_COL);
    draw_palette();

    // ---- Эмблема (слева) ----
    const char *want = (argc > 1) ? argv[1] : "cat";
    if (str_eq(want, "eye")) {
        draw_art(EYE_LINES, EYE_COLORS,
                 (int)(sizeof(EYE_LINES) / sizeof(EYE_LINES[0])));
    } else if (str_eq(want, "ankh")) {
        draw_art(ANKH_LINES, ANKH_COLORS,
                 (int)(sizeof(ANKH_LINES) / sizeof(ANKH_LINES[0])));
    } else {
        draw_art(CAT_LINES, CAT_COLORS,
                 (int)(sizeof(CAT_LINES) / sizeof(CAT_LINES[0])));
    }

    // Курсор в последнюю строку
    uint32_t cursor_row = (rows > 1) ? (rows - 1) : 1;
    printf("\x1B[%d;1H", cursor_row);

    return 0;
}
