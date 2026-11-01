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

#include "include-kernel/gdt.h"
#include <stdint.h>

struct gdt_entry gdt[6];
struct gdt_ptr gp;
struct tss_entry tss __attribute__((aligned(8)));

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran){
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

void tss_init(void){
    tss.ss0 = 0x10;
    tss.esp0 = 0x90000;
    asm volatile("ltr %%ax" : : "a"(0x28));
}

void update_tss_esp0(uint32_t esp0){
    tss.esp0 = esp0;
}

void gdt_init(void){
    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xCF); // код ядра
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xCF); // данные ядра
    gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xCF); // код пользователя
    gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xCF); // данные пользователя
    gdt_set_gate(5, (uint32_t)&tss, sizeof(tss)-1, 0x89, 0x00); // TSS

    gp.limit = sizeof(gdt) - 1;
    gp.base = (uint32_t)&gdt;
    asm volatile("lgdt (%0)" : : "r"(&gp));

    asm volatile(
        "push $0x10\n"
        "pop %%ds\n"
        "push $0x10\n"
        "pop %%es\n"
        "push $0x10\n"
        "pop %%fs\n"
        "push $0x10\n"
        "pop %%gs\n"
        "push $0x10\n"
        "pop %%ss\n"
        : : : "memory"
    );
}
