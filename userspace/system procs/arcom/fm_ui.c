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

#include <stdint.h>
#include <stddef.h>
#include "include/fm_platform.h"

// Буфер кадра — собираем весь ANSI в один блок, потом сбрасываем одним write
static char     g_out[16384];
static unsigned g_pos = 0;

static void out_reset(void) { g_pos = 0; }

static void out_flush(void)
{
    if (g_pos) { fm_console_write(g_out, g_pos); g_pos = 0; }
}

static void out_put(char c)
{
    if (g_pos + 1 >= sizeof(g_out)) out_flush();
    g_out[g_pos++] = c;
}

static void out_str(const char *s)
{
    while (*s) out_put(*s++);
}

static void out_uint(unsigned v)
{
    char b[12]; int n = 0;
    if (!v) { out_put('0'); return; }
    while (v) { b[n++] = '0' + (v % 10); v /= 10; }
    while (n) out_put(b[--n]);
}

// -------- Публичный примитив --------

// Не отправляем \x1B[?25l: ARHeOS kconsole не понимает "?" внутри CSI
// и вывел бы "25l" на экран. Курсор оставляем видимым, но прибиваем
// его к безопасной позиции в draw-цикле.
void ui_init(void)
{
    out_reset();
    // Явно ставим чёрный фон ДО очистки: kconsole_clear_screen видит
    // console_bg_active == true и заливает fill_rect'ом, а не fb_clear
    // (который при загруженных обоях восстановил бы обои на весь экран).
    out_str("\x1B[40m\x1B[2J\x1B[H");
    out_flush();
}

void ui_shutdown(void)
{
    out_reset();
    out_str("\x1B[0m\x1B[2J\x1B[H");
    out_flush();
}

void ui_begin_frame(void) { out_reset(); out_str("\x1B[H"); }
void ui_end_frame(void)   { out_str("\x1B[0m"); out_flush(); }

void ui_goto(int x, int y)
{
    out_str("\x1B[");
    out_uint((unsigned)(y + 1));
    out_put(';');
    out_uint((unsigned)(x + 1));
    out_put('H');
}

// Цвета (ANSI SGR): fg 0..7, bg 0..7, bold
void ui_color(int fg, int bg, int bold)
{
    out_str("\x1B[");
    if (bold) { out_put('1'); out_put(';'); }
    out_uint((unsigned)(fg + 30));
    out_put(';');
    out_uint((unsigned)(bg + 40));
    out_put('m');
}

void ui_reset_color(void) { out_str("\x1B[0m"); }
void ui_inverse(void)     { out_str("\x1B[7m"); }
void ui_puts(const char *s) { out_str(s); }
void ui_putc(char c) { out_put(c); }
void ui_pad(int count) { while (count-- > 0) out_put(' '); }

// Стереть до конца строки (не двигает курсор, не скроллит)
void ui_clear_eol(void) { out_str("\x1B[K"); }

// Записать строку, при необходимости обрезав, добить пробелами до max-1.
// НЕ трогает последнюю ячейку строки — безопасно и для последней строки экрана.
void ui_putn(const char *s, int max)
{
    if (max <= 1) return;
    int limit = max - 1;
    int i = 0;
    while (s[i] && i < limit) { out_put(s[i]); i++; }
    while (i < limit) { out_put(' '); i++; }
}

// Как ui_putn, но добивает остаток строки через \x1B[K, чтобы визуально
// залить всю ширину цветом фона. Безопасно в самой нижней строке — ARHeOS
// не сдвигает курсор при K, а вызовы fill_rect/clear_tile проходят по всей
// строке без wrap'а и без scroll_up.
void ui_putn_eol(const char *s, int max)
{
    if (max <= 1) { out_str("\x1B[K"); return; }
    int limit = max - 1;
    int i = 0;
    while (s[i] && i < limit) { out_put(s[i]); i++; }
    out_str("\x1B[K");
}
