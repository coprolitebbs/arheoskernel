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

#ifndef PORTS_IO_H
#define PORTS_IO_H

#include <stdint.h>

void outb(uint16_t port, uint8_t value);

uint8_t inb(uint16_t port);

void outw(uint16_t port, uint16_t value);

uint16_t inw(uint16_t port);

void io_wait(void);

void insw(uint16_t port, void *addr, uint32_t count);

void outsw(uint16_t port, const void *addr, uint32_t count);

void outl(uint16_t port, uint32_t value);

uint32_t inl(uint16_t port);

#endif
