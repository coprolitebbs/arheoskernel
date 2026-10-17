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

#ifndef LOCALE_H
#define LOCALE_H

#include "locale_types.h"
#include "font.h"

#define LOCALE_POOL_MAX 8


void             locale_registry_init(void);
int              locale_register(const locale_t *loc);
const locale_t  *locale_by_code(const char *code);
const locale_t  *locale_get_active(void);
int              locale_set_active(const char *code);
locale_t *locale_alloc(const char *code, const char *display_name, const font_t *font);

//Доступ к слоту пула по индексу. Возвращает 0, если слот пуст или индекс вне границ
const locale_t  *locale_get_by_index(uint8_t idx);

uint32_t locale_resolve_key(const locale_t *loc, uint8_t scancode, bool shift);

void             font_registry_init(void);
int              font_register(const font_t *font);
const font_t    *font_by_name(const char *name);

#endif
