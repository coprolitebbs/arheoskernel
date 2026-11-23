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

#include "include-kernel/isr.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/draw.h"
#include "include-kernel/task.h"
#include "include-kernel/lib.h"
#include "include-kernel/ports_io.h"
#include "include-kernel/debug.h"
#include "include-kernel/comdebug.h"
#include "drivers/include_drivers/drv_format.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/kbd_translate.h"
#include "include-kernel/locale.h"
#include "include-kernel/font_vga.h"
#include "include-kernel/kpanic.h"
#include "include-kernel/kload_screen.h"

//Флаг IRQ12 (PS/2 мышь / AUX). Ставится при каждом принятом байте мыши.
//Раньше опрос был полностью poll-овым (драйвер мыши читал 0x60 напрямую)
//и глотал скан-коды клавиатуры, попавшие в буфер между опросами — терялись
//нажатия. Теперь порт принадлежит ядру: обработчики IRQ1/IRQ12 раскладывают
//байты сами, AUX-байты копятся в кольце ниже и отдаются драйверу мыши через
//kernel_mouse_read_byte. Флаг остался для диагностики и совместимости
volatile uint8_t mouse_irq_fired = 0;

volatile uint8_t floppy_irq_fired = 0;
volatile uint32_t tick_count = 0;
volatile uint8_t kernel_schedule_lock = 0;
volatile uint8_t init_kernel_schedule_lock = 0;
volatile uint8_t kernel_panic_active = 0;
volatile uint8_t stdin_cancel_flag = 0;

//Внешние символы из isr.asm (обработчики)
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

extern void syscall_handler_asm(void); //int 0x80

//Вспомогательная функция для заполнения IDT
static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

//Инициализация IDT
void idt_init(void)
{
    for (int i = 0; i < 256; i++)
    {
        idt_set_gate(i, 0, 0, 0);
    }

    //Исключения (0–31)
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10,  0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11,  0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12,  0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13,  0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14,  0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15,  0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16,  0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17,  0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18,  0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19,  0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20,  0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21,  0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22,  0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23,  0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24,  0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25,  0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26,  0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27,  0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28,  0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29,  0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30,  0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31,  0x08, 0x8E);

    //Аппаратные прерывания (32–47)
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);  //таймер
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);  //клава
    idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    //Системный вызов (int 0x80)
    idt_set_gate(0x80, (uint32_t)syscall_handler_asm, 0x08, 0xEE); //ловушка

    //Загружаем IDT
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    asm volatile("lidt (%0)" : : "r"(&idtp));
}


//Перенастройка PIC
void pic_init(void)
{
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    //outb(0x21, 0xFE);  // разрешить только IRQ0
    outb(0x21, 0xB8); // разрешить IRQ0 и IRQ1 и IRQ6
    //outb(0xA1, 0xFF);
    outb(0xA1, 0xEF); // на PIC2 разрешить только IRQ12 (PS/2 мышь)
}



//GENERAL PROTECTION FAULT
void isr_halt13(struct regs *r)
{
    //Окно паники рисуется через примитивы графического слоя
    //Флаг глушит sys_yield до конца жизни системы (мы все равно в halt-цикле)
    kernel_panic_active = 1;

    panic_field_t fields[6] =
    {
        { "ERR",   r->err_code          },
        { "EIP",   r->eip               },
        { "CS",    r->cs                },
        { "ESP",   r->user_esp          },
        { "Sched", (uint32_t)shedule_count },
        { "Ticks", tick_count           },
    };

    draw_kernel_panic("GENERAL PROTECTION FAULT", fields, 6);

    com_puts("[KERNEL] General Protection Fault! Halted.\n");
    for(;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

void isr_double_fault(struct regs *r)
{
    (void)r;
    //com_puts("[KERNEL] DOUBLE FAULT! Halted.\n");
    __asm__ __volatile__("cli");
    for(;;) __asm__ __volatile__("hlt");
}

//PAGE FAULT
void isr_pagefault(struct regs *r)
{
    //та же защита окна паники, что в isr_halt13: переключение контекста
    //из обработчика фолта запрещено до конца работы
    kernel_panic_active = 1;

    uint32_t cr2, cr3;
    __asm__ __volatile__("mov %%cr2,%0":"=r"(cr2));
    __asm__ __volatile__("mov %%cr3,%0":"=r"(cr3));

    //Переключаемся на kernel_directory: там LFB точно замаплен (vmm_map_pages добавлял его туда)
    //Панику рисуем из гарантированно-валидного адресного пространства
    uint32_t kd_phys = (uint32_t)kernel_directory & 0xFFFFF000;
    if (kd_phys != 0 && (cr3 & 0xFFFFF000) != kd_phys)
    {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(kd_phys) : "memory");
    }

    panic_field_t fields[8] =
    {
        { "CR2",   cr2                                                    },
        { "ERR",   r->err_code                                            },
        { "EIP",   r->eip                                                 },
        { "CS",    r->cs                                                  },
        { "ESP",   r->user_esp                                            },
        { "CR3",   cr3                                                    },
        { "TaskCR3", current_task ? current_task->page_dir_phys : 0       },
        { "Sched", (uint32_t)shedule_count                                },
    };

    draw_kernel_panic("PAGE FAULT", fields, 8);

    //Дублируем поля паники в COM
    com_puts("[KERNEL] PAGE FAULT! Halted.\n");
    com_puts("  CR2=");    com_puthex(cr2);
    com_puts(" ERR=");     com_puthex(r->err_code);
    com_puts(" EIP=");     com_puthex(r->eip);
    com_puts(" CS=");      com_puthex(r->cs);
    com_puts(" CR3=");     com_puthex(cr3);
    com_puts(" TaskCR3="); com_puthex(current_task ? current_task->page_dir_phys : 0);
    com_puts("\n");
    for(;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}




//Кольцо AUX-байтов мыши. Контроллер i8042 выдает байты мыши и клавиатуры
//через один порт 0x60, различие — только бит AUX (bit5) в статусе (0x64),
//поэтому порт принадлежит ядру, а не драйверу мыши.
//Писатель сюда — обработчики IRQ1/IRQ12 (вложений нет: стаб не включает IF),
//читатель — обработчик драйвера мыши через kernel_mouse_read_byte.
//Классический SPSC-кольцевой буфер, head/tail — свободно бегущие индексы без .wrap-веток
#define MOUSE_RX_SIZE 128   //степень двойки
#define MOUSE_RX_MASK (MOUSE_RX_SIZE - 1)

static volatile uint8_t  mouse_rx[MOUSE_RX_SIZE];
static volatile uint32_t mouse_rx_head = 0;     //пишет только ISR
static volatile uint32_t mouse_rx_tail = 0;     //пишет только читатель (драйвер)
static volatile uint32_t mouse_rx_dropped = 0;  //потери при переполнении кольца

//Забрать один AUX-байт из кольца. 0 — байт в *out, -1 — кольцо пусто.
//Вызывается из poll драйвера мыши (контекст задач и системных вызовов)
int kernel_mouse_read_byte(uint8_t *out)
{
    if (!out) return -1;
    uint32_t t = mouse_rx_tail;
    if (t == mouse_rx_head) return -1;
    *out = mouse_rx[t & MOUSE_RX_MASK];
    mouse_rx_tail = t + 1;
    return 0;
}

//Разложить один байт выходного буфера i8042 по назначению:
//AUX — в кольцо мыши, клавиатурный — хоткеям и драйверу клавиатуры
static void i8042_dispatch_byte(uint8_t st, uint8_t b)
{
    if (st & 0x20)                     //bit5 статуса = AUX: байт от мыши
    {
        uint32_t h = mouse_rx_head;
        if ((uint32_t)(h - mouse_rx_tail) >= MOUSE_RX_SIZE)
        {
            //Кольцо переполнено (драйвер давно не звал свой обработчик) — байт теряем
            //Граница пакета развалится, драйвер пересинхронизируется по bit3
            mouse_rx_dropped++;
            return;
        }
        mouse_rx[h & MOUSE_RX_MASK] = b;
        mouse_rx_head = h + 1;
        mouse_irq_fired = 1;
    }
    else
    {
        //Хоткей смены раскладки читает только модификаторы и переключает
        //раскладку, сам скан-код при этом идет дальше обычным путем
        kbd_hotkey_process(b);
        if (kbd_driver_api && kbd_driver_api->irq_callback)
        {
            kbd_driver_api->irq_callback(b);
        }
    }
}

//Вычитать и разложить все байты, успевшие попасть в выходной буфер.
//Общий тракт для IRQ1 и IRQ12: каждый обработчик забирает все, что есть,
//и раскладывает по биту AUX — мышиные байты не теряются в обработчике
//клавиатуры и наоборот. Ограничение итераций, поскольку вызывается из ISR
void kernel_i8042_drain(void)
{
    int guard = 16;
    while (guard--)
    {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;       //bit0 статуса = OBF: буфер пуст
        i8042_dispatch_byte(st, inb(0x60));
    }
}

void irq33_handler(struct regs *r)
{
    (void)r;
    //Скан-коды уходят клавиатуре, случайные AUX-байты — в кольцо мыши
    kernel_i8042_drain();
    //EOI в PIC1 после разбора буфера
    outb(0x20, 0x20);
}




void irq6_handler(struct regs *r)
{
    (void)r;
    //Фиксируем, что прерывание от дисковода успешно прилетело
    floppy_irq_fired = 1;
    //посылаем сигнал EOI (End of Interrupt) в контроллер PIC1 (порт 0x20)
    outb(0x20, 0x20);//io_wait();
}


//IRQ12 — PS/2 мышь (порт AUX контроллера i8042)
//Байты мыши кладем в кольцо (драйвер заберет их через read_byte),
//случайный клавиатурный байт не теряем — отдаем клавиатурному тракту
void irq44_handler(struct regs *r)
{
    (void)r;
    mouse_irq_fired = 1;
    kernel_i8042_drain();
    //EOI в PIC2 (IRQ8-15 живут там) и в PIC1 — иначе шина мыши "залипнет"
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}




//Основной обработчик прерываний
void isr_handler(struct regs *r)
{
    if (r->int_no == 8)
    {
        isr_double_fault(r);
        for(;;);
    }

    //Исключения (0–31) — критическая ошибка
    if (r->int_no < 32)
    {
        if (r->int_no == 14)
        {
            //PAGE FAULT
            isr_pagefault(r);
            for(;;);

        }
        if (r->int_no == 13)
        {
            isr_halt13(r);
        }
    }

    if (r->int_no == 33)
    {
        irq33_handler(r);
        return;
    }

    if (r->int_no == 38)
    {
        irq6_handler(r); //Вызываем обработчик (он выставит floppy_irq_fired = 1 и пошлет EOI)
        return;// r;
    }

    if (r->int_no == 44){
        irq44_handler(r); //IRQ12: PS/2 мышь — флаг mouse_irq_fired + EOI
        return;
    }

    //Аппаратные прерывания (32–47)
    if (r->int_no >= 32 && r->int_no < 48)
    {
        //Обработка конкретных IRQ
        if (r->int_no == 32)
        {
            outb(0x20, 0x20);

            //Увеличиваем системные тики
            tick_count++;

            //пока держится блокировка планировщика (смена режима, spawn)
            //— рисовать из IRQ нельзя: LFB и CRTC в этот момент перепрограммируются, состояние графического слоя недействительно
            if (kernel_schedule_lock == 0)
            {
                //Обновляем мигание курсора TTY
                kconsole_update_cursor();

                if ((tick_count & 5) == 0)
                {
                    kernel_load_animation_next_step();
                }
            }

            if (kernel_schedule_lock == 1) return;
            //Вызываем планировщик
            schedule(r);
            return;   //после schedule() управление не вернётся
        }
        else
        {
            //Для остальных IRQ — просто EOI
            if (r->int_no >= 40)
            {
                outb(0xA0, 0x20);
            }
            outb(0x20, 0x20);
        }
        return;
    }
    //Системный вызов (int 0x80)
    if (r->int_no == 0x80)
    {
        syscall_handler(r);
        return;
    }


}

void pit_init(void)
{
    uint32_t divisor = 1193180 / 100;

    outb(0x43,0x36);

    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}


void sys_yield(void)
{
    //Во время паники квант не отдаем - обработчик фолта рисует окно через слой,
    //и переключение задачи бросило бы его недорисованным, а возврат в задачу по старому esp
    //сыграл бы фолтящую инструкцию заново
    //Ждущие циклы драйверов в этом контексте просто крутят спин
    if (kernel_panic_active) return;
    schedule(NULL);
}

/*
void pit_init(void)
{
    // Устанавливаем максимальный делитель 65535 (0xFFFF).
    // Это снизит частоту таймера со 100 Гц до классических ~18.2 Гц.
    // Прерывания станут прилетать в 5.5 раз РЕЖЕ.
    uint32_t divisor = 65535;

    outb(0x43, 0x36);

    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}
*/
