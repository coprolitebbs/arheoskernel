#ifndef KBD_DRIVER_H
#define KBD_DRIVER_H

#include <stdint.h>
#include "../../../include-kernel/bootinfo.h"
#include "../../include_drivers/drv_format.h"


extern kbd_driver_api_t *api_ptr;

void driver_main(struct boot_info *boot);

kbd_driver_api_t *driver_get_api(void);


void kbd_irq_callback(uint8_t scancode);
char kbd_get_char_blocked(void);
int  kbd_has_chars(void);


#endif
