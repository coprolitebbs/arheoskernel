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

#include "include/kbd_driver.h"
#include "include/kbd_internal.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"

kbd_driver_api_t api;
kbd_driver_api_t *api_ptr;

description_driver_api_t descr_api;


// Внутренний кольцевой буфер символов драйвера
kbd_event_t kbd_event_buf[KBD_BUFFER_SIZE];
kbd_event_t *kbd_event_buf_ptr;
volatile uint32_t kbd_head = 0;
volatile uint32_t kbd_tail = 0;

static bool driver_caps_lock_state = false;
static bool current_hardware_led_state = false;
static uint8_t led_command_state = 0;

//  ----- внутренние функции  -----

static void kbd_wait_write(void) {
    uint32_t timeout = 50000;
    while ((inb(KBD_STATUS_PORT) & 0x02) && timeout--) {
        asm volatile("nop");
    }
}

static void kbd_update_leds(void) {
    if (led_command_state != 0) return;

    kbd_wait_write();
    led_command_state = 1;
    outb(KBD_DATA_PORT, 0xED);
}


//  -----  внешние функции  -----

// Асинхронный коллбек прерывания (вызывается из ядра по IRQ1)
void kbd_irq_callback(uint8_t scancode){
    // Если Capslock, зажечь индикатор
    if (scancode == 0xFA){
        if (led_command_state == 1){
            uint8_t led_mask = 0;
            if (driver_caps_lock_state){
                led_mask |= 0x04;
            }
            kbd_wait_write();
            led_command_state = 2;
            outb(KBD_DATA_PORT, led_mask);
            return;
        }
        else if (led_command_state == 2){
            led_command_state = 0;
            current_hardware_led_state = driver_caps_lock_state;
            return;
        }
    }
    if (scancode == 0x3A){
        driver_caps_lock_state = !driver_caps_lock_state;
        return;
    }
    //Префикс расширенных клавиш 0xE0 имеет взведенный бит 0x80,
    //но его нельзя отсекать как событие отпускания! Пропускаем его в буфер.
    if (scancode != 0xE0){
        //Если прилетел скан-код отпускания обычной клавиши (старший бит 0x80 взведен),
        //упаковываем его со специальным типом (type = 0) и пропускаем, либо обрабатываем.
        uint32_t next_head = (kbd_head + 1) % KBD_BUFFER_SIZE;
        if (next_head == kbd_tail) return; //Переполнение

        if (scancode & 0x80){
            // Событие отпускания клавиши (Release)
            kbd_event_buf_ptr[kbd_head].scancode = scancode & 0x7F; // Чистый код клавиши
            kbd_event_buf_ptr[kbd_head].type = 0;                  // Type 0 = Отпущена
        } else {
            // Событие нажатия клавиши (Press)
            kbd_event_buf_ptr[kbd_head].scancode = scancode;
            kbd_event_buf_ptr[kbd_head].type = 1;                  // Type 1 = Нажата
        }
        kbd_head = next_head;
        return;
    }
    //Если прилетел чистый префикс 0xE0 — пишем его в буфер как нажатие (type = 1)
    uint32_t next_head = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next_head == kbd_tail) return;
    kbd_event_buf_ptr[kbd_head].scancode = 0xE0;
    kbd_event_buf_ptr[kbd_head].type = 1;
    kbd_head = next_head;
}




//Блокирующее чтение символа (Вызывается системным вызовом SYS_READ)
uint32_t kbd_get_event_blocked(void){
    //api_ptr->debug_val1++;
    while (1){
        if (driver_caps_lock_state != current_hardware_led_state){
            kbd_update_leds();
        }
        if (kbd_tail != kbd_head) {

            kbd_event_t ev = kbd_event_buf_ptr[kbd_tail];
            kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE; //Неделимый инкремент

            //Упаковываем младший байт = scancode, второй байт = type
            uint32_t packed_value = ev.scancode | (ev.type << 8);
            return packed_value; //Данные возвращаются через регистр EAX
        }
        if (api_ptr->yield_ptr) api_ptr->yield_ptr();
    }
    return 0;
}


//Неблокирующая проверка наличия символов (Аналог KBHIT)
int kbd_has_events(void){
    if (driver_caps_lock_state != current_hardware_led_state){
        kbd_update_leds();
    }
    return (kbd_tail != kbd_head) ? 1 : 0;
}


void kbd_init(struct boot_info *boot){
    (void)boot;
    kbd_event_buf_ptr = kbd_event_buf;
    kbd_head = 0;
    kbd_tail = 0;
    memset(kbd_event_buf_ptr, 0, KBD_BUFFER_SIZE);

    driver_caps_lock_state = false;
    current_hardware_led_state = true;
    led_command_state = 0;
}


void driver_main(struct boot_info *boot){
    kbd_init(boot);

    description_driver_api_t *descr_api_ptr = &descr_api;
    descr_api_ptr->type = DESCR_KEYBOARD_DRIVER;

    api_ptr = &api;

    api_ptr->irq_callback = kbd_irq_callback;
    api_ptr->has_chars = kbd_has_events;

    api_ptr->get_char_blocked = kbd_get_event_blocked;

    descr_api_ptr->struct_ptr = (void *)&api;
}


description_driver_api_t *driver_get_api(void){
    return &descr_api;
}


