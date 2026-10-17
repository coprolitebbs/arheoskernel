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

#include "include-kernel/font.h"
#include "include-kernel/lib.h"

#define FONT_REGISTRY_MAX 8

static const font_t *g_fonts[FONT_REGISTRY_MAX];
static int           g_font_count;

void font_registry_init(void){
    for (int i = 0; i < FONT_REGISTRY_MAX; i++) g_fonts[i] = 0;
    g_font_count = 0;
}

int font_register(const font_t *font){
    if (!font || g_font_count >= FONT_REGISTRY_MAX) return -1;
    //Защита от двойной регистрации
    for (int i = 0; i < g_font_count; i++)
        if (g_fonts[i] == font) return i;
    g_fonts[g_font_count] = font;
    return g_font_count++;
}

const font_t *font_by_name(const char *name){
    if (!name) return 0;
    for (int i = 0; i < g_font_count; i++){
        const char *a = g_fonts[i]->name;
        const char *b = name;
        while (*a && *a == *b){ a++; b++; }
        if (*a == *b) return g_fonts[i];
    }
    return 0;
}

const font_glyph_t *font_lookup(const font_t *f, uint32_t cp){
    if (!f) return 0;
    for (uint32_t i = 0; i < f->glyph_count; i++){
        if (f->glyphs[i].codepoint == cp) return &f->glyphs[i];
    }
    return f->fallback;
}
