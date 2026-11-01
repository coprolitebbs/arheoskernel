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

#ifndef UDEVICES_STRUCT_H
#define UDEVICES_STRUCT_H

//Структура для пользовательских программ и для системных вызовов ядра
typedef struct {
    uint32_t vid;
    uint32_t pid;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  port;
    uint32_t block_id;
    char     manufacturer[32];
    char     product[32];
    char     serial[20];
} __attribute__((packed)) u_usb_dev_t;




#endif //UDEVICES_STRUCT_H
