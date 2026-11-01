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

#ifndef LOCALE_TYPES_H
#define LOCALE_TYPES_H

#include "font.h"

typedef struct {
    uint32_t normal[128];
    uint32_t shift [128];
} locale_keymap_t;

typedef struct {
    const char   *code;         //"en", "ru", "de"
    const char   *display_name; //"English", "Русский", ...
    const font_t *font;         //активный шрифт локали

    locale_keymap_t  *keymap;
} locale_t;



#endif
