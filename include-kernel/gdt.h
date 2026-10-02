#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void tss_init(void);
void update_tss_esp0(uint32_t esp0);
void gdt_init(void);

struct gdt_entry{
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry{
    uint32_t link;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

//Объявления (extern)
extern struct gdt_entry gdt[6];
extern struct gdt_ptr gp;
extern struct tss_entry tss;

#endif
