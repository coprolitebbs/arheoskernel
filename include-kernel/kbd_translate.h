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

#ifndef KBD_TRANSLATE_H
#define KBD_TRANSLATE_H
#include <stdint.h>

#define HOTKEY_NONE         0
#define HOTKEY_ALT_SHIFT    1
#define HOTKEY_CTRL_SHIFT   2
#define HOTKEY_ALT_ALONE    3
#define HOTKEY_CAPS_LOCK    4
#define HOTKEY_CTRL_SPACE   5

extern uint8_t locale_hotkey;

// Возвращает:
// > 0 : печатный Unicode codepoint
// = 0 : непечатное событие (модификатор, стрелка)
uint32_t kbd_translate_event(uint32_t packed_event);

// Сбросить модификаторы (например, при переключении TTY/задачи)
void kbd_translate_reset(void);

//Обработка скан-кода хоткеем смены раскладки. Вызывается из irq33_handler
//первой, до передачи скан-кода в драйвер клавы. Читает только состояние
//модификаторов, сам скан-код не поглощает и не меняет.
void kbd_hotkey_process(uint8_t scancode);

#endif
