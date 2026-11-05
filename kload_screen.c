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

/*
 * kload_screen.c - ARHeOS загрузочный splash screen.
 *
 *
 *   +-- ----------- ------------------------------------------------+
 *   |    .    .    .     .       .         .   .      .    .        |
 *   |                                                               |
 *   |             +---------------------------+                     |
 *   |             |                           |                     |
 *   |             |       A R H e O S         |                     |
 *   |             |                           |                     |
 *   |             | Alternate Retro           |                     |
 *   |             | Home entertainment        |                     |
 *   |             | Operating System v0.1     |                     |
 *   |             +---------------------------+                     |
 *   |             ============================                      |
 *   |             +---------------------------+                     |
 *   |             |  Загрузка:                |                     |
 *   |             |   *  Kernel image loaded  |                     |
 *   |             |   *  Memory management... |                     |
 *   |             |   *  Interrupt handlers...|                     |
 *   |             |   *  Task scheduler...    |                     |
 *   |             |   *  Loading drivers...   |                     |
 *   |             |    ooo  [/]               |                     |
 *   |             |  [=====>     ]            |                     |
 *   |             +---------------------------+                     |
 *   |                                                               |
 *   |          ARHeOS (c) 2026 - Навеяно старыми ОС                 |
 *   |              Загрузка, ожидайте...                            |
 *   +---------------------------------------------------------------+
 */

#include "include-kernel/kload_screen.h"
#include "include-kernel/draw.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/lib.h"
#include "include-kernel/pmm.h"
#include "include-kernel/vmm.h"
#include "include-kernel/task.h"
#include "include-kernel/kconsole.h"


//Положение отдельных модулей

static bool     anim_active = false;
static uint32_t anim_step   = 0;
static uint32_t screen_w    = 0;
static uint32_t screen_h    = 0;

static uint32_t header_y  = 0;
static uint32_t panel_x   = 0;
static uint32_t panel_y   = 0;
static uint32_t bar_x     = 0;
static uint32_t bar_y     = 0;
static uint32_t bar_w     = 0;

static const char spinner_chars[SPINNER_COUNT] = { '|', '/', '-', '\\' };

bool load_screen_can_paint = true;

static char     log_lines[LOG_CAPACITY][LOG_COLS + 1];
static uint32_t log_head  = 0;   //индекс самой старой записи
static uint32_t log_count = 0;   //сколько записей реально лежит в кольце



//Хелперы

//Цветной символ(RGB = 0x00RRGGBB, прозрачный фон)
static void color_char(uint32_t x, uint32_t y, uint8_t ch, uint32_t rgb)
{
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >>  8) & 0xFF;
    uint8_t b =  rgb        & 0xFF;
    _draw_char(x, y, ch, r, g, b, 1, 0, 0, 0);
}

//Цветные строки
static void color_string(uint32_t x, uint32_t y, const char *str, uint32_t rgb)
{
    while (*str)
    {
        color_char(x, y, (uint8_t)*str, rgb);
        x += 8;
        str++;
    }
}

//Цветная строка с обрезкой по ширине
static void color_string_clipped(uint32_t x, uint32_t y, const char *str, uint32_t rgb, uint32_t max_x)
{
    while (*str && x + 8 <= max_x)
    {
        color_char(x, y, (uint8_t)*str, rgb);
        x += 8;
        str++;
    }
}

//Цветные полосы по центру
static void color_string_centered(uint32_t y, const char *str, uint32_t rgb)
{
    uint32_t len = 0;
    while (str[len]) len++;
    uint32_t w = len * 8;
    uint32_t x = (screen_w > w) ? (screen_w - w) / 2 : 0;
    color_string(x, y, str, rgb);
}

//Однопиксельный прямоугольник
static void rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t col)
{
    if (w == 0 || h == 0) return;
    kernel_fill_rect(x,         y,         w, 1, col);
    kernel_fill_rect(x,         y + h - 1, w, 1, col);
    if (h >= 2)
    {
        kernel_fill_rect(x,         y + 1, 1, h - 2, col);
        kernel_fill_rect(x + w - 1, y + 1, 1, h - 2, col);
    }
}

//Двухцветная панель с заголовком
static void panel_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t outer, uint32_t inner)
{
    rect_outline(x, y, w, h, outer);
    if (w > 6 && h > 6)
    {
        rect_outline(x + 2, y + 2, w - 4, h - 4, inner);
    }
}

//Интерполяция между двумя цветами 0xRRGGBB
static uint32_t lerp_rgb(uint32_t a, uint32_t b, uint32_t t, uint32_t tmax)
{
    if (tmax == 0) tmax = 1;
    int32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int32_t r  = ar + (br - ar) * (int32_t)t / (int32_t)tmax;
    int32_t g  = ag + (bg - ag) * (int32_t)t / (int32_t)tmax;
    int32_t bl = ab + (bb - ab) * (int32_t)t / (int32_t)tmax;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

//Бэкграунд с вертикальным градиентом
static void draw_background(void)
{
    uint32_t bands = 24;
    uint32_t bh = screen_h / bands;
    if (bh == 0) bh = 1;
    for (uint32_t i = 0; i < bands; i++)
    {
        uint32_t col = lerp_rgb(C_BG_TOP, C_BG_BOTTOM, i, bands - 1);
        kernel_fill_rect(0, i * bh, screen_w, bh + 1, col);
    }
    if (bands * bh < screen_h)
    {
        kernel_fill_rect(0, bands * bh, screen_w, screen_h - bands * bh, C_BG_BOTTOM);
    }
}

//Звездное поле
static void draw_stars(void)
{
    uint32_t rng = 0x4A3B2C1Du;
    uint32_t max_y = (header_y > 16) ? header_y - 8 : 8;
    for (uint32_t i = 0; i < STAR_COUNT; i++)
    {
        rng = rng * 1103515245u + 12345u;
        uint32_t x = (rng >> 8) % (screen_w ? screen_w : 1);
        rng = rng * 1103515245u + 12345u;
        uint32_t y = 8 + ((rng >> 8) % max_y);
        uint32_t bright = (rng >> 16) & 1u;
        uint32_t col = bright ? C_STAR_BRIGHT : C_STAR;
        uint32_t sz  = bright ? 2 : 1;
        kernel_fill_rect(x, y, sz, sz, col);
    }
}

//Перерисовать область лога из кольцевого буфера
//Вызывается из draw_static() и из load_screen_add_string()
//Безопасно, если draw_static ещё не вызывался - тогда panel_y == 0,
//ничего не рисует и просто выходит
static void redraw_log(void)
{
    if (panel_y == 0 || bar_w == 0) return;

    uint32_t ry      = panel_y + 14;
    uint32_t log_x   = panel_x + 20;
    uint32_t log_y   = ry + LOG_FIRST_Y_OFF;
    uint32_t log_w   = bar_w;
    uint32_t log_h   = LOG_VISIBLE * LOG_LINE_H;
    uint32_t max_x   = log_x + log_w;
    //Заливаем фон под логом, чтобы стереть предыдущую отрисовку
    kernel_fill_rect(log_x, log_y, log_w, log_h + 8, C_PANEL);
    //Сколько строк реально показать
    uint32_t visible = log_count;
    if (visible > LOG_VISIBLE) visible = LOG_VISIBLE;
    //Пропускаем самые старые, если в буфере их больше, чем помещается
    uint32_t skip = (log_count > LOG_VISIBLE) ? (log_count - LOG_VISIBLE) : 0;
    for (uint32_t i = 0; i < visible; i++)
    {
        uint32_t slot = (log_head + skip + i) % LOG_CAPACITY;
        color_string_clipped(log_x, log_y + i * LOG_LINE_H,
                             log_lines[slot], C_TEXT, max_x);
    }
}

//Статичная часть

static void draw_static(void)
{
    //Градиентный бэкграунд
    draw_background();

    //Метрики
    uint32_t margin = (screen_w >= 800) ? 48 : 24;

    //Высота заголовка
    header_y = 32;

    //Звездочки
    draw_stars();

    //Верхние и нижние звездочки
    kernel_fill_rect(0, 0, screen_w, 2, C_ACCENT);
    kernel_fill_rect(0, screen_h - 2, screen_w, 2, C_ACCENT);

    //Заголовочная панель
    uint32_t hw = 420;
    if (hw > screen_w - 2 * margin) hw = screen_w - 2 * margin;
    uint32_t hh = 86;
    uint32_t hx = (screen_w - hw) / 2;

    kernel_fill_rect(hx, header_y, hw, hh, C_PANEL);
    panel_border(hx, header_y, hw, hh, C_PANEL_OUT, C_PANEL_IN);
    //Андерлайн
    kernel_fill_rect(hx, header_y + hh + 1, hw, 2, C_ACCENT);

    //Заголовок
    {
        const char *title = "A R H e O S";
        uint32_t tl = 0;
        while (title[tl]) tl++;
        uint32_t tw = tl * 8;
        uint32_t tx = (screen_w - tw) / 2;
        uint32_t ty = header_y + 14;
        color_string(tx + 1, ty + 1, title, C_TITLE_SHAD);
        color_string(tx,     ty,     title, C_TITLE);
    }

    //Дополнительные строки
    color_string_centered(header_y + 40, "Alternate Retro Home entertainment Operating System", C_TEXT_DIM);
    color_string_centered(header_y + 56, "System Bootstrap v0.1", C_TEXT_DIM);

    //Панель статуса
    uint32_t pw = 480;
    if (pw > screen_w - 2 * margin) pw = screen_w - 2 * margin;
    uint32_t ph = 170;
    panel_x = (screen_w - pw) / 2;
    panel_y = header_y + hh + 22;

    kernel_fill_rect(panel_x, panel_y, pw, ph + 400, C_PANEL);
    panel_border(panel_x, panel_y, pw, ph + 400, C_PANEL_OUT, C_PANEL_IN);

    //Заголовок лога
    //(сами строки лога рисуются через redraw_log() в конце функции)
    uint32_t rx = panel_x + 20;
    uint32_t ry = panel_y + 14;
    color_string(rx, ry, "Загрузка:", C_TEXT_BRIGHT);

    //Место прогрессбара
    bar_x = panel_x + 20;
    bar_w = pw - 40;
    bar_y = panel_y + ph - 40 + 400;
    uint32_t bar_h = 20;

    kernel_fill_rect(bar_x, bar_y, bar_w, bar_h, C_BAR_TRACK);
    rect_outline(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, C_BAR_EDGE);

    //Футер
    {
        uint32_t fy = panel_y + ph + 18;
        color_string_centered(fy,      "ARHeOS (c) 2026  -  Навеяно старыми ОС", C_TEXT_DIM);
        color_string_centered(fy + 14, "Загрузка, ожидайте...", C_TEXT_DIM);
    }

    //Лог поверх статики, в свою область
    redraw_log();
}

//Динамическая часть ортрисовки

static void draw_dynamic(void)
{
    uint32_t bar_h    = 20;
    uint32_t inner_x  = bar_x + 2;
    uint32_t inner_y  = bar_y + 2;
    uint32_t inner_w  = bar_w - 4;
    uint32_t inner_h  = bar_h - 4;

    //Бегающий прогрессбар
    if(anim_active)
    {
        kernel_fill_rect(inner_x, inner_y, inner_w, inner_h, C_BAR_TRACK);
        uint32_t seg_w = inner_w / BAR_SEGMENTS;
        if (seg_w < 2) seg_w = 2;
        uint32_t used_w   = seg_w * BAR_SEGMENTS;
        uint32_t offset_x = inner_x + (inner_w - used_w) / 2;
        if (offset_x < inner_x + 1) offset_x = inner_x + 1;

        uint32_t head = anim_step % BAR_SEGMENTS;

        for (uint32_t i = 0; i < BAR_SEGMENTS; i++)
        {

            uint32_t dist = (head + BAR_SEGMENTS - i) % BAR_SEGMENTS;

            uint32_t col;
            if (dist < COMET_HEAD_LEN)                               col = C_BAR_CORE;
            else if (dist < COMET_HEAD_LEN + COMET_HOT_LEN)          col = C_BAR_HOT;
            else if (dist < COMET_HEAD_LEN + COMET_HOT_LEN + COMET_BODY_LEN)
                col = C_BAR_FILL;
            else                                                      col = C_BAR_TRACK;

            if (col == C_BAR_TRACK) continue;

            uint32_t x = offset_x + i * seg_w;
            kernel_fill_rect(x, inner_y, seg_w - 1 > 0 ? seg_w - 1 : 1,inner_h, col);
        }
    }
    //Вертящийся спиннер
    /*{
        char sp = spinner_chars[anim_step % SPINNER_COUNT];
        uint32_t sp_x = bar_x + bar_w - 22;
        uint32_t sp_y = bar_y - 22;
        kernel_fill_rect(sp_x - 3, sp_y - 2, 14, 12, C_PANEL);
        color_char(sp_x, sp_y, (uint8_t)sp, C_ACCENT);
    }*/

    //Три пульсирующих точки
    /*{
        uint32_t dots_y = bar_y - 22;
        uint32_t dots_x = panel_x + 20;
        for (uint32_t i = 0; i < PULSE_DOTS; i++){
            uint32_t phase = (anim_step + i * 3) % 9;
            uint32_t col = (phase < 3) ? C_ACCENT
                         : (phase < 6) ? C_ACCENT2
                         :                C_TEXT_DIM;
            kernel_fill_rect(dots_x + i * 8, dots_y + 4, 4, 4, col);
        }
    }*/
}


static bool screen_is_damaged(void)
{
    if (!boot_info || !boot_info->framebuffer) return true;
    uint8_t *p = (uint8_t*)boot_info->framebuffer;   /* pixel (0,0) */

    if (boot_info->bpp == 32)
    {
        return (*(uint32_t*)p & 0x00FFFFFFu) != C_ACCENT;
    }
    if (boot_info->bpp == 24)
    {
        uint32_t px = ((uint32_t)p[2] << 16) |
                      ((uint32_t)p[1] <<  8) |
                      (uint32_t)p[0];
        return px != C_ACCENT;
    }
    if (boot_info->bpp == 16)
    {
        //Считаем RGB565 из C_ACCENT, чтобы смена палитры не ломала ничего
        uint32_t r = (C_ACCENT >> 16) & 0xFF;
        uint32_t g = (C_ACCENT >>  8) & 0xFF;
        uint32_t b =  C_ACCENT        & 0xFF;
        uint16_t expected = (uint16_t)(((r >> 3) << 11) |
                                       ((g >> 2) <<  5) |
                                       (b >> 3));
        return *(uint16_t*)p != expected;
    }
    return true;   //неизвестный bpp
}


//Очистить лог полностью
static void log_clear(void)
{
    log_head  = 0;
    log_count = 0;
    for (uint32_t i = 0; i < LOG_CAPACITY; i++)
    {
        log_lines[i][0] = '\0';
    }
}

//Запихать одну уже готовую физическую строку в кольцо
static void log_push_line(const char *line, uint32_t len)
{
    //Обрезаем висящие пробелы справа
    while (len > 0 && line[len - 1] == ' ') len--;
    //Обрезаем ведущие пробелы
    while (len > 0 && line[0] == ' ')
    {
        line++;
        len--;
    }
    //Пустые строки не пихаем
    if (len == 0) return;
    if (len > LOG_COLS) len = LOG_COLS;

    uint32_t slot;
    if (log_count < LOG_CAPACITY)
    {
        slot = (log_head + log_count) % LOG_CAPACITY;
        log_count++;
    }
    else
    {
        //Кольцо переполнено - затираем самую старую запись
        slot = log_head;
        log_head = (log_head + 1) % LOG_CAPACITY;
    }

    for (uint32_t i = 0; i < len; i++)
    {
        log_lines[slot][i] = line[i];
    }
    log_lines[slot][len] = '\0';
}

//Разбить строку на физические строки с переносом по словам и запихать в кольцо
static void log_push_wrapped(const char *text)
{
    if (!text || !text[0]) return;

    char     line[LOG_COLS + 1];
    uint32_t len        = 0;
    uint32_t last_space = 0;
    bool     has_space  = false;

    while (*text)
    {
        //Явный перенос строки
        if (*text == '\n')
        {
            log_push_line(line, len);
            len = 0;
            last_space = 0;
            has_space = false;
            text++;
            continue;
        }

        //Строка заполнена - переносим
        if (len >= LOG_COLS)
        {
            if (has_space && last_space > 0)
            {
                //Перенос по слову: режем по последнему пробелу
                log_push_line(line, last_space);
                uint32_t tail_len = len - last_space - 1;
                for (uint32_t i = 0; i < tail_len; i++)
                {
                    line[i] = line[last_space + 1 + i];
                }
                len = tail_len;
                last_space = 0;
                has_space  = false;
                //Заново ищем последний пробел в перенесённом хвосте
                for (uint32_t i = 0; i < len; i++)
                {
                    if (line[i] == ' ')
                    {
                        last_space = i;
                        has_space  = true;
                    }
                }
            }
            else
            {
                //Некуда переносить - режем жёстко
                log_push_line(line, len);
                len = 0;
            }
            continue;
        }

        //Запоминаем позицию последнего пробела
        if (*text == ' ')
        {
            last_space = len;
            has_space  = true;
        }
        line[len++] = *text++;
    }

    log_push_line(line, len);
}

//Мини-vsnprintf для форматирования строк лога
//Поддержка: %s %d %u %x %X %c %p %%
static void boot_vsnprintf(char *out, uint32_t out_size, const char *fmt,
                           __builtin_va_list args)
{
    uint32_t o = 0;
    if (!out || out_size == 0) return;

    while (*fmt && o + 1 < out_size)
    {
        if (*fmt != '%')
        {
            out[o++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '\0') break;

        if (*fmt == 's')
        {
            const char *s = __builtin_va_arg(args, const char *);
            if (!s) s = "(null)";
            while (*s && o + 1 < out_size)
            {
                out[o++] = *s++;
            }
        }
        else if (*fmt == 'd')
        {
            int32_t v = __builtin_va_arg(args, int32_t);
            char tmp[12];
            int32_t ti = 0;
            if (v < 0)
            {
                if (o + 1 < out_size) out[o++] = '-';
                v = -v;
            }
            if (v == 0) tmp[ti++] = '0';
            while (v > 0)
            {
                tmp[ti++] = '0' + (v % 10);
                v /= 10;
            }
            while (ti > 0 && o + 1 < out_size)
            {
                out[o++] = tmp[--ti];
            }
        }
        else if (*fmt == 'u')
        {
            uint32_t v = __builtin_va_arg(args, uint32_t);
            char tmp[12];
            int32_t ti = 0;
            if (v == 0) tmp[ti++] = '0';
            while (v > 0)
            {
                tmp[ti++] = '0' + (v % 10);
                v /= 10;
            }
            while (ti > 0 && o + 1 < out_size)
            {
                out[o++] = tmp[--ti];
            }
        }
        else if (*fmt == 'x' || *fmt == 'X')
        {
            uint32_t v = __builtin_va_arg(args, uint32_t);
            const char *hex = (*fmt == 'X') ? "0123456789ABCDEF"
                              : "0123456789abcdef";
            char tmp[12];
            int32_t ti = 0;
            if (v == 0) tmp[ti++] = '0';
            while (v > 0)
            {
                tmp[ti++] = hex[v & 0xF];
                v >>= 4;
            }
            while (ti > 0 && o + 1 < out_size)
            {
                out[o++] = tmp[--ti];
            }
        }
        else if (*fmt == 'c')
        {
            out[o++] = (char)__builtin_va_arg(args, int);
        }
        else if (*fmt == 'p')
        {
            uint32_t v = __builtin_va_arg(args, uint32_t);
            if (o + 3 < out_size)
            {
                out[o++] = '0';
                out[o++] = 'x';
            }
            const char *hex = "0123456789abcdef";
            char tmp[9];
            int32_t ti = 0;
            if (v == 0) tmp[ti++] = '0';
            while (v > 0)
            {
                tmp[ti++] = hex[v & 0xF];
                v >>= 4;
            }
            while (ti > 0 && o + 1 < out_size)
            {
                out[o++] = tmp[--ti];
            }
        }
        else if (*fmt == '%')
        {
            out[o++] = '%';
        }
        else
        {
            //Неизвестный спецификатор - выводим буквально
            if (o + 1 < out_size) out[o++] = '%';
            if (o + 1 < out_size) out[o++] = *fmt;
        }
        fmt++;
    }
    out[o] = '\0';
}

//Внешний API

void kernel_load_animation_start(void)
{
    if (!boot_info) return;
    if (boot_info->width  == 0 || boot_info->height == 0) return;
    if (boot_info->bpp    != 16 &&
            boot_info->bpp    != 24 &&
            boot_info->bpp    != 32) return;
    if (boot_info->framebuffer == 0) return;

    screen_w = boot_info->width;
    screen_h = boot_info->height;

    if (screen_w < 400 || screen_h < 300) return;

    anim_active = true;
    anim_step   = 0;

    draw_static();
    draw_dynamic();
}

void kernel_load_animation_next_step(void)
{
    if(!load_screen_can_paint) return;
    if (!anim_active) return;

    if (screen_is_damaged())
    {
        draw_static();
    }

    anim_step++;
    draw_dynamic();
}

void kernel_load_animation_stop_nohide(void)
{
    if (!anim_active) return;
    anim_active = false;
    kernel_fill_rect(bar_x + 2, bar_y + 2, bar_w - 4, 20 - 4, C_BAR_TRACK);
}


void kernel_load_animation_stop(void)
{
    kernel_load_animation_stop_nohide();

    if (current_tty && fb && fb->clear)
    {
        kconsole_clear_screen();
    }
    else
    {
        kernel_fill_rect(0, 0, screen_w, screen_h, 0x00000000);
    }
}


void load_screen_add_string(const char *fmt, ...)
{
    if (!anim_active) return;
    if (!fmt) return;

    //Форматируем во временный буфер на стеке
    char formatted[256];
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    boot_vsnprintf(formatted, (uint32_t)sizeof(formatted), fmt, args);
    __builtin_va_end(args);

    //Разбиваем на физические строки и пихаем в кольцо
    log_push_wrapped(formatted);

    //Если нельзя писать в фреймбуфер - сообщение уже в кольце,
    //отрисуется на следующем тике анимации
    if (!load_screen_can_paint) return;

    //Если экран был стёрт кем-то снаружи - восстанавливаем статику
    //(которая включает redraw_log). Иначе перерисовываем только лог
    if (screen_is_damaged())
    {
        draw_static();
    }
    else
    {
        redraw_log();
    }
}


