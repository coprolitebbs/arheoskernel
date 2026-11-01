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

#include "include-kernel/kbd_translate.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/locale.h"

//Состояние модификаторов для трансляции скан-кодов в печатные символы
static bool shift_active = false;
static bool ctrl_active  = false;
static bool caps_active  = false;

//Состояние модификаторов для детектора хоткея
static bool hotkey_alt_held   = false;
static bool hotkey_shift_held = false;
static bool hotkey_ctrl_held  = false;
static bool hotkey_triggered  = false;

uint8_t locale_hotkey = HOTKEY_ALT_SHIFT;


uint32_t kbd_translate_event(uint32_t packed_event){
    uint8_t sc   = (uint8_t)(packed_event & 0xFF);
    uint8_t type = (uint8_t)((packed_event >> 8) & 0xFF);

    if (sc == 0x2A || sc == 0x36){ shift_active = (type == 1); return 0; }
    if (sc == 0x1D)               { ctrl_active  = (type == 1); return 0; }
    if (sc == 0x3A && type == 1)  { caps_active = !caps_active; return 0; }
    if (type != 1) return 0;
    if (sc >= 128) return 0;
    //Источник истины о раскладке — активная локаль ядра
    //locale_resolve_key сам уйдёт в g_default_keymap (US), если в keymap
    //активной локали для этого скан-кода ничего не прописано
    const locale_t *loc = locale_get_active();
    uint32_t cp = locale_resolve_key(loc, sc, shift_active);
    if (cp == 0) return 0;

    //Caps Lock переворачивает регистр букв. Работаем в codepoint-пространстве,
    //потому что именно так их отдаёт локальный слой.
    if (caps_active){
        if (cp >= 'a' && cp <= 'z')            cp -= 0x20; //a-z -> A-Z
        else if (cp >= 'A' && cp <= 'Z')       cp += 0x20; //A-Z -> a-z
        else if (cp >= 0x0430 && cp <= 0x044F) cp -= 0x20; //а-я -> А-Я
        else if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20; //А-Я -> а-я
        else if (cp == 0x0451)                 cp = 0x0401; //ё -> Ё
        else if (cp == 0x0401)                 cp = 0x0451; //Ё -> ё
    }

    (void)ctrl_active;
    //return (char)cp866_from_codepoint(cp);
    return cp;
}

void kbd_translate_reset(void){
    shift_active = false;
    ctrl_active  = false;
    caps_active  = false;
    hotkey_alt_held   = false;
    hotkey_shift_held = false;
    hotkey_ctrl_held  = false;
    hotkey_triggered  = false;
}


//Обходим пул локалей по кругу, пропуская пустые слоты, при нахождении первой инициализированной — делаем её активной
//в текущем TTY и синхронизируем глобальную локаль ядра
static void kbd_layout_switch(void){
    if (!current_tty) return;

    uint8_t cur = current_tty->active_layout;
    for (uint8_t step = 1; step <= LOCALE_POOL_MAX; step++){
        uint8_t next = (uint8_t)((cur + step) % LOCALE_POOL_MAX);
        const locale_t *loc = locale_get_by_index(next);
        if (!loc) continue;
        //kprintf("[HOTKEY] switch: %d -> %d (%s)\n", cur, next, loc->code);
        current_tty->active_layout = next;
        locale_set_active(loc->code);
        return;
    }
}


//Обработчик хоткея. Вызывается из irq33_handler на каждый скан-код
void kbd_hotkey_process(uint8_t scancode){
    if (locale_hotkey == HOTKEY_NONE) return;
    //0xE0 — префикс расширенных клавиш, не скан-код. Пропускаем без изменений.
    if (scancode == 0xE0) return;
    //Разбираем скан-код: старший бит означает отпускание для обычных клавиш
    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;
    //Обновляем модификаторы, участвующие в хоткее
    if (code == 0x38){ hotkey_alt_held   = !is_release; }
    if (code == 0x2A || code == 0x36){ hotkey_shift_held = !is_release; }
    if (code == 0x1D){ hotkey_ctrl_held  = !is_release; }

    //Определяем, должен ли хоткей сработать именно в этом событии.
    //Для модификаторных комбинаций это «оба зажаты прямо сейчас», для
    //связки Ctrl+Space — «в этом событии пришло нажатие Space при зажатом Ctrl».
    bool combo_now = false;
    switch (locale_hotkey){
        case HOTKEY_ALT_SHIFT:
            combo_now = hotkey_alt_held && hotkey_shift_held;
            break;
        case HOTKEY_CTRL_SHIFT:
            combo_now = hotkey_ctrl_held && hotkey_shift_held;
            break;
        case HOTKEY_ALT_ALONE:
            combo_now = hotkey_alt_held;
            break;
        case HOTKEY_CTRL_SPACE:
            //Реагируем только на момент нажатия Space (скан-код 0x39) при уже зажатом Ctrl.
            //Порядок «Space раньше Ctrl» хоткеем не считается — это совпадает с macOS
            //и защищает от ложных срабатываний при обычном наборе текста.
            combo_now = hotkey_ctrl_held && !is_release && (code == 0x39);
            break;
    default:
        break;
}
    //Срабатываем на фронте - один раз при входе в комбинацию, не повторяемся до её снятия.
    //Для Ctrl+Space это дает повторное срабатывание при повторном нажатии Space,
    //пока Ctrl остается зажатым — циклический перебор раскладок
    if (combo_now && !hotkey_triggered){
        kbd_layout_switch();
        hotkey_triggered = true;
    } else if (!combo_now){
        hotkey_triggered = false;
    }
}

