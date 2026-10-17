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

#include "include-kernel/font_vga.h"
#include "include-kernel/font.h"
#include "include-kernel/locale.h"
#include "include-kernel/font_data.h"
#include "include-kernel/kconsole.h"

//Мост "CP866-байт -> Unicode codepoint" для существующего шрифта ядра.
//ASCII-диапазон 0x00..0x7F мапится как есть, остальное — по таблице
//0 означает "нет отображения, глиф пропустить"

static const uint16_t cp866_upper[128] = {
    //0x80..0x8F: А Б В Г Д Е Ж З И Й К Л М Н О П
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    //0x90..0x9F: Р С Т У Ф Х Ц Ч Ш Щ Ъ Ы Ь Э Ю Я
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    //0xA0..0xAF: а б в г д е ж з и й к л м н о п
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    //0xB0..0xDF: псевдографика
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    //0xE0..0xEF: р с т у ф х ц ч ш щ ъ ы ь э ю я
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
    //0xF0..0xFF: Ё ё ≥ ≤ ⌠ ⌡ ÷ ≈ ° ∙ · √ ⁿ ² ■ NBSP
    0x0401, 0x0451, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};





static font_glyph_t g_vga_glyphs[VGA_GLYPH_MAX];
static uint32_t g_vga_glyph_count;

static font_t g_vga_font = {
    .name          = "vga-8x8",
    .cell_width    = 8,
    .cell_height   = 8,
    .bytes_per_row = 1,
    .glyphs        = g_vga_glyphs,
    .glyph_count   = 0,
    .fallback      = 0,
};


void font_vga_init(void){
    font_registry_init();
    locale_registry_init();

    g_vga_glyph_count = 0;
    g_vga_font.glyph_count = 0;
    g_vga_font.fallback    = 0;

    for (uint32_t b = 0; b < 256; b++){
        uint32_t cp = (b < 0x80) ? b : cp866_upper[b - 0x80];
        if (cp == 0) continue;

        g_vga_glyphs[g_vga_glyph_count].codepoint = cp;
        g_vga_glyphs[g_vga_glyph_count].width     = 8;
        g_vga_glyphs[g_vga_glyph_count].height    = 8;
        g_vga_glyphs[g_vga_glyph_count].rows      = &font_data[b][0];
        g_vga_glyph_count++;
    }
    g_vga_font.glyph_count = g_vga_glyph_count;

    //Fallback — квадратик U+25A0. Он гарантированно есть в таблице
    for (uint32_t i = 0; i < g_vga_glyph_count; i++){
        if (g_vga_glyphs[i].codepoint == 0x25A0){
            g_vga_font.fallback = &g_vga_glyphs[i];
            break;
        }
    }
    //На всякий случай — если по какой-то причине не нашли
    if (!g_vga_font.fallback && g_vga_glyph_count > 0)
        g_vga_font.fallback = &g_vga_glyphs[0];

    font_register(&g_vga_font);

    //locale_alloc("en", "English",&g_vga_font, &g_keymap_en);
    //locale_alloc("ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9",&g_vga_font, &g_keymap_ru);
    //Первая зарегистрированная (en) автоматически становится активной.
}


void init_standard_locales(void){
    kprintf("Init standard locales...\n");
    //kprintf("[LOC] g_vga_font=%p\n", (void*)&g_vga_font);
    locale_alloc("en", "English",&g_vga_font);
    locale_alloc("ru", "Русский",&g_vga_font);

    //kprintf("[LOC] active=%s font=%p glyph_count=%d\n",active->code, (void*)active->font, active->font->glyph_count);
}
