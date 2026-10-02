#ifndef ISR_H
#define ISR_H

#include <stdint.h>
#include "task.h"

extern int xoffs;
extern volatile uint32_t tick_count;
extern volatile uint8_t floppy_irq_fired;
extern volatile uint8_t kernel_schedule_lock;
extern volatile uint8_t init_kernel_schedule_lock;
extern volatile uint8_t stdin_cancel_flag;
//extern volatile uint32_t tty_foreground_pid;

//Структура состояния регистров, передаваемая из обработчика
struct regs{
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, user_esp, user_ss;
};


//Структура IDT
struct idt_entry{
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));


struct idt_ptr{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));



struct ipc_message{
    uint32_t type;

    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void idle_loop(void);

void isr_pagefault(struct regs *r);

//Инициализация IDT (заполнение таблицы и загрузка)
void idt_init(void);

//Инициализация PIC (перенастройка контроллеров прерываний)
void pic_init(void);

//Основной обработчик прерываний (вызывается из asm-общего обработчика)
void isr_handler(struct regs *r);

void pit_init(void);

void sys_yield(void);

#endif
