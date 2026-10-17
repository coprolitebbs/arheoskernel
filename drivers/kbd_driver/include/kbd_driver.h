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

#ifndef KBD_DRIVER_H
#define KBD_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"


extern kbd_driver_api_t *api_ptr;

void driver_main(struct boot_info *boot);

description_driver_api_t *driver_get_api(void);


void kbd_irq_callback(uint8_t scancode);
char kbd_get_char_blocked(void);
int  kbd_has_chars(void);


#endif
