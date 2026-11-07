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

volatile uint8_t floppy_irq_fired = 0;
volatile uint32_t tick_count = 0;
volatile uint8_t kernel_schedule_lock = 0;
volatile uint8_t init_kernel_schedule_lock = 0;
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
    outb(0xA1, 0xFF);
}



//GENRAL PROTECTION FAULT
void isr_halt13(struct regs *r)
{
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





//=======================================================================
//  Редактор строки cooked-режима ядра (kbd line discipline)
//=======================================================================
//Полноценная линейная дисциплина в прерывании клавиатуры: набор символов
//с вставкой в произвольную позицию, Backspace, Del, стрелки, Home/End,
//Ctrl+C (отмена ввода), Enter (сдача строки читателю через SYS_READ).
//Буфер строки — current_tty->line_buffer (UTF-8), позиция курсора —
//line_cursor_pos в БАЙТАХ. Экранная позиция любой ячейки буфера
//вычисляется через kconsole_get_render_coords (UTF-8-aware, перенос
//строк считается от input_start_x/y, зафиксированных на старте сессии).
//=======================================================================

//Флаг RAW-режима клавиатуры (дублируется из userspace/include/syscall.h)
#ifndef TTY_KBD_RAW
#define TTY_KBD_RAW 0x0008
#endif

//Декодирует codepoint в line_buffer[idx], возвращает длину через out_len.
//На битых данных отдает сам байт длиной 1 — редактор не зависает
static uint32_t kbd_utf8_decode_at(uint32_t idx, uint32_t *out_len)
{
    uint8_t *buf = (uint8_t *)current_tty->line_buffer;
    *out_len = 1;
    if (idx >= current_tty->copied_bytes) return ' ';

    uint8_t b = buf[idx];
    uint32_t need, cp, min;
    if ((b & 0x80) == 0x00) return b;
    if ((b & 0xE0) == 0xC0) { need = 1; cp = b & 0x1F; min = 0x80;    }
    else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0F; min = 0x800;   }
    else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07; min = 0x10000; }
    else return b;

    if (current_tty->copied_bytes - idx < need + 1) return b;
    for (uint32_t k = 1; k <= need; k++)
    {
        if ((buf[idx + k] & 0xC0) != 0x80) return b;
        cp = (cp << 6) | (uint32_t)(buf[idx + k] & 0x3F);
    }
    if (cp < min) return b;
    *out_len = need + 1;
    return cp;
}

//Индекс предыдущего символа UTF-8 (вызывать при pos > 0)
static uint32_t kbd_prev_char_pos(uint32_t pos)
{
    uint8_t *buf = (uint8_t *)current_tty->line_buffer;
    uint32_t p = pos - 1;
    while (p > 0 && ((buf[p] & 0xC0) == 0x80)) p--;
    return p;
}

//Индекс следующего символа UTF-8 (вызывать при pos < copied_bytes)
static uint32_t kbd_next_char_pos(uint32_t pos)
{
    uint8_t *buf = (uint8_t *)current_tty->line_buffer;
    uint32_t p = pos + 1;
    while (p < current_tty->copied_bytes && ((buf[p] & 0xC0) == 0x80)) p++;
    return p;
}

//Затирает знакоместо по БАЙТОВОМУ индексу в line_buffer: восстанавливает фон
//под символом (wallpaper через clear_tile, иначе заливка фоном) и чистит матрицу
static void kbd_clear_cell_at(uint32_t byte_idx)
{
    uint32_t cx, cy;
    kconsole_get_render_coords(byte_idx, &cx, &cy);
    uint32_t cell_x = cx / current_tty->cell_w;
    uint32_t cell_y = cy / current_tty->cell_h;
    if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols)
    {
        current_tty->matrix[cell_y][cell_x].codepoint = ' ';
    }
    if (fb && fb->clear_tile)
    {
        fb->clear_tile(cx, cy, console_bgcolor);
    }
    else if (fb && fb->draw_codepoint)
    {
        fb->draw_codepoint(cx, cy, ' ', console_bgcolor);
    }
}

//Рисует символ по БАЙТОВОМУ индексу в line_buffer (тот же паттерн фона,
//что и в kernel_stdout_write_codepoint)
static void kbd_draw_char_at(uint32_t byte_idx, uint32_t cp)
{
    uint32_t cx, cy;
    kconsole_get_render_coords(byte_idx, &cx, &cy);
    uint32_t cell_x = cx / current_tty->cell_w;
    uint32_t cell_y = cy / current_tty->cell_h;
    uint32_t cell_bg = console_bg_active ? console_bgcolor : 0xFFFFFFFF;

    if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols)
    {
        current_tty->matrix[cell_y][cell_x].codepoint = cp;
        current_tty->matrix[cell_y][cell_x].color     = console_fgcolor;
        current_tty->matrix[cell_y][cell_x].bg        = cell_bg;
    }
    if (cell_bg != 0xFFFFFFFF && fb && fb->fill_rect)
    {
        fb->fill_rect(cx, cy, current_tty->cell_w, current_tty->cell_h, cell_bg);
    }
    if (fb && fb->draw_codepoint)
    {
        fb->draw_codepoint(cx, cy, cp, console_fgcolor);
    }
}

//Стирает блочный курсор в ячейке buffer_idx, восстанавливая лежащий под ним символ
static void kbd_erase_cursor_at(uint32_t byte_idx)
{
    uint32_t cx, cy;
    kconsole_get_render_coords(byte_idx, &cx, &cy);
    uint32_t cell_x = cx / current_tty->cell_w;
    uint32_t cell_y = cy / current_tty->cell_h;
    uint32_t orig_char = ' ';
    uint32_t orig_bg   = 0xFFFFFFFF;
    if (cell_y < current_tty->console_rows && cell_x < current_tty->console_cols)
    {
        orig_char = current_tty->matrix[cell_y][cell_x].codepoint;
        orig_bg   = current_tty->matrix[cell_y][cell_x].bg;
    }
    if (orig_bg != 0xFFFFFFFF && fb && fb->fill_rect)
    {
        fb->fill_rect(cx, cy, current_tty->cell_w, current_tty->cell_h, orig_bg);
    }
    else if (fb && fb->clear_tile)
    {
        fb->clear_tile(cx, cy, console_bgcolor);
    }
    else if (fb && fb->draw_codepoint)
    {
        fb->draw_codepoint(cx, cy, ' ', console_bgcolor);
    }
    if (orig_char != ' ' && orig_char != '\0')
    {
        if (fb && fb->draw_codepoint) fb->draw_codepoint(cx, cy, orig_char, console_fgcolor);
    }
}

//Рисует блочный курсор в ячейке buffer_idx и перезапускает фазу мигания
static void kbd_draw_cursor_at(uint32_t byte_idx)
{
    uint32_t cx, cy;
    kconsole_get_render_coords(byte_idx, &cx, &cy);
    if (fb && fb->draw_codepoint)
    {
        fb->draw_codepoint(cx, cy, current_tty->cursor_code, console_fgcolor);
    }
    current_tty->cursor_visible  = true;
    current_tty->last_blink_tick = tick_count;
}

//Синхронизирует курсор вывода ядра (cursor_x/y) с экранной позицией byte_idx.
//Нужно, чтобы последующий вывод программ и перевод строки на Enter
//начинались ровно с того места, куда указывает курсор ввода
static void kbd_place_output_cursor(uint32_t byte_idx)
{
    uint32_t cx, cy;
    kconsole_get_render_coords(byte_idx, &cx, &cy);
    current_tty->cursor_x = cx;
    current_tty->cursor_y = cy;
}

//Перерисовывает хвост строки от byte_idx до конца и затирает ячейки,
//освободившиеся после удаления символов (в границах old_end_idx)
static void kbd_redraw_tail(uint32_t from_idx, uint32_t old_end_idx)
{
    uint32_t i = from_idx;
    while (i < current_tty->copied_bytes)
    {
        uint32_t len = 1;
        uint32_t cp  = kbd_utf8_decode_at(i, &len);
        kbd_draw_char_at(i, cp);
        i += len;
    }
    for (uint32_t c = current_tty->copied_bytes; c < old_end_idx; c++)
    {
        kbd_clear_cell_at(c);
    }
}

//Вставка символа (codepoint) в позицию курсора со сдвигом хвоста вправо
static void kbd_line_insert_cp(uint32_t cp)
{
    uint8_t utf8[4];
    int n = 0;
    if (cp < 0x80)
    {
        utf8[n++] = (uint8_t)cp;
    }
    else if (cp < 0x800)
    {
        utf8[n++] = (uint8_t)(0xC0 | (cp >> 6));
        utf8[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        utf8[n++] = (uint8_t)(0xE0 | (cp >> 12));
        utf8[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        utf8[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    else
    {
        utf8[n++] = (uint8_t)(0xF0 | (cp >> 18));
        utf8[n++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        utf8[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        utf8[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    //Резервируем минимум 1 байт под '\n' и 1 под '\0'
    if (current_tty->copied_bytes + (uint32_t)n > LINE_BUFFER_SIZE - 2) return;

    //Сначала подтираем блочный курсор в текущей ячейке,
    //иначе после сдвига символов под блоком останется артефакт
    kbd_erase_cursor_at(current_tty->line_cursor_pos);

    uint32_t old_end = current_tty->copied_bytes;
    for (uint32_t i = current_tty->copied_bytes; i > current_tty->line_cursor_pos; i--)
    {
        current_tty->line_buffer[i + n - 1] = current_tty->line_buffer[i - 1];
    }
    for (int k = 0; k < n; k++)
    {
        current_tty->line_buffer[current_tty->line_cursor_pos + k] = (char)utf8[k];
    }
    current_tty->copied_bytes += (uint32_t)n;
    current_tty->line_buffer[current_tty->copied_bytes] = 0;

    kbd_redraw_tail(current_tty->line_cursor_pos, old_end);
    current_tty->line_cursor_pos += (uint32_t)n;
    kbd_place_output_cursor(current_tty->line_cursor_pos);
    kbd_draw_cursor_at(current_tty->line_cursor_pos);
}

//Backspace: удаление символа СЛЕВА от курсора, хвост сдвигается и перерисовывается.
//При переходе через границу строк kconsole_get_render_coords сама вернет
//экранную позицию на предыдущую строку (у самой правой ячейки строки выше)
static void kbd_line_backspace(void)
{
    if (current_tty->line_cursor_pos == 0) return;

    uint32_t del_pos = kbd_prev_char_pos(current_tty->line_cursor_pos);
    uint32_t del_len = current_tty->line_cursor_pos - del_pos;
    if (del_len == 0) return;

    kbd_erase_cursor_at(current_tty->line_cursor_pos);
    uint32_t old_end = current_tty->copied_bytes;
    for (uint32_t i = del_pos; i + del_len < current_tty->copied_bytes; i++)
    {
        current_tty->line_buffer[i] = current_tty->line_buffer[i + del_len];
    }
    current_tty->copied_bytes -= del_len;
    current_tty->line_buffer[current_tty->copied_bytes] = 0;
    current_tty->line_cursor_pos = del_pos;

    kbd_redraw_tail(del_pos, old_end);
    kbd_place_output_cursor(current_tty->line_cursor_pos);
    kbd_draw_cursor_at(current_tty->line_cursor_pos);
}

//Del: удаление символа СПРАВА от курсора со сдвигом и перерисовкой хвоста
static void kbd_line_delete(void)
{
    if (current_tty->line_cursor_pos >= current_tty->copied_bytes) return;

    uint32_t del_len = 1;
    kbd_utf8_decode_at(current_tty->line_cursor_pos, &del_len);

    kbd_erase_cursor_at(current_tty->line_cursor_pos);
    uint32_t old_end = current_tty->copied_bytes;
    for (uint32_t i = current_tty->line_cursor_pos;
         i + del_len < current_tty->copied_bytes; i++)
    {
        current_tty->line_buffer[i] = current_tty->line_buffer[i + del_len];
    }
    if (current_tty->copied_bytes > del_len) current_tty->copied_bytes -= del_len;
    else current_tty->copied_bytes = 0;
    current_tty->line_buffer[current_tty->copied_bytes] = 0;

    kbd_redraw_tail(current_tty->line_cursor_pos, old_end);
    kbd_place_output_cursor(current_tty->line_cursor_pos);
    kbd_draw_cursor_at(current_tty->line_cursor_pos);
}

//Стирает всю область строки ввода на экране (от начала строки до позиции
//за последним символом включительно) — используется при отмене ввода
static void kbd_erase_input_area(void)
{
    for (uint32_t i = 0; i <= current_tty->copied_bytes; i++)
    {
        kbd_clear_cell_at(i);
    }
}

//Ctrl+C: отмена ввода. Строка на экране подтирается, читателю сдается
//пустая строка ('\n' в буфере) — для вызывающего это обычный пустой Enter
static void kbd_session_cancel(void)
{
    kbd_erase_input_area();
    current_tty->cursor_x = current_tty->input_start_x;
    current_tty->cursor_y = current_tty->input_start_y;
    kernel_stdout_write_char('^', console_fgcolor);
    kernel_stdout_write_char('C', console_fgcolor);
    kernel_stdout_write_char('\n', console_fgcolor);

    current_tty->line_buffer[0] = '\n';
    current_tty->line_buffer[1] = 0;
    current_tty->copied_bytes    = 1;
    current_tty->line_cursor_pos = 0;
    current_tty->line_ready      = true;
    stdin_cancel_flag            = 1;
}

//Enter: сдача строки. '\n' дописывается в буфер (u_read пользователя
//завершает строку именно по '\n'), курсор вывода переводится на новую строку
static void kbd_line_commit(void)
{
    kbd_erase_cursor_at(current_tty->line_cursor_pos);
    if (current_tty->copied_bytes < LINE_BUFFER_SIZE - 1)
    {
        current_tty->line_buffer[current_tty->copied_bytes++] = '\n';
        current_tty->line_buffer[current_tty->copied_bytes] = 0;
    }
    //Курсор вывода — в конец содержимого строки, затем честный перевод строки
    kbd_place_output_cursor(current_tty->copied_bytes - 1);
    kernel_stdout_write_char('\n', console_fgcolor);
    current_tty->line_ready = true;
}

//Caps Lock: переворот регистра букв (та же логика, что в kbd_translate_event)
static uint32_t kbd_apply_caps(uint32_t cp, bool caps)
{
    if (!caps) return cp;
    if (cp >= 'a' && cp <= 'z')            return cp - 0x20;
    if (cp >= 'A' && cp <= 'Z')            return cp + 0x20;
    if (cp >= 0x0430 && cp <= 0x044F)      return cp - 0x20;
    if (cp >= 0x0410 && cp <= 0x042F)      return cp + 0x20;
    if (cp == 0x0451)                      return 0x0401;
    if (cp == 0x0401)                      return 0x0451;
    return cp;
}


void irq33_handler(struct regs *r)
{
    //Аппаратно вычитываем скан-код клавиши из порта контроллера i8042
    uint8_t scancode = inb(0x60);
    //Посылаем сигнал EOI (End of Interrupt) контроллеру прерываний PIC,
    //чтобы клавиатурная шина не вешала систему на время выполнения кода
    outb(0x20, 0x20);
    //Обрабатываем хоткей смены раскладки. Он читает только
    //состояние модификаторов и переключает раскладку при нажатии комбинации,
    //сам скан-код при этом идёт дальше обычным путём
    kbd_hotkey_process(scancode);
    //Передаем скан-код в драйвер клавы
    //Это наполнит кольцевой буфер kbd_event_buf, и RAW-читатели (Сехмет) увидят нажатие
    if (kbd_driver_api && kbd_driver_api->irq_callback)
    {
        kbd_driver_api->irq_callback(scancode);
    }
    if (current_tty && current_tty->foreground_pid != 0)
    {
        //Если терминал захвачен активным шеллом/процессом Ring 3 — выходим
        //Оставим только обработку переключения TTY по комбинациям клавиш
        return;
    }
    //Fallback ядра (cooked-редактор строки)
    if (!current_tty) return;
    static bool caps_lock_active = false;
    static bool ctrl_pressed = false;
    extern bool shift_pressed;

    if (scancode == 0x1D)      ctrl_pressed = true;
    else if (scancode == 0x9D) ctrl_pressed = false;

    //Если текущая задача читает клавиатуру в RAW-режиме (Сехмет и т.п.) —
    //она сама обрабатывает ввод и рисует свой курсор, ядерный редактор молчит
    if (current_task && (current_task->tty_flags & TTY_KBD_RAW)) return;

    //Пока собранная строка не отдана читателю — редактирование закрыто
    if (current_tty->line_ready) return;

    //Ctrl+C — отмена ввода: строка подтирается, читателю сдается пустая.
    //Действуем только если сессия ввода реально активна — иначе курсор вывода
    //прыгнул бы в устаревшую позицию посреди вывода работающей программы
    if (ctrl_pressed && scancode == 0x2E)
    {
        if (current_tty->line_session_active)
        {
            kbd_session_cancel();
        }
        return;
    }

    //Первое нажатие в новой строке: фиксируем начало сессии ввода
    //в текущей позиции курсора вывода (сразу за промптом программы)
    if (!current_tty->line_session_active)
    {
        kconsole_arm_input_session();
    }

    //Навигация и правка по make-кодам (у CF-18 нет numpad, конфликтов кодов нет)
    switch (scancode)
    {
    case 0x4B: //Стрелочка <-
        if (current_tty->line_cursor_pos > 0)
        {
            kbd_erase_cursor_at(current_tty->line_cursor_pos);
            current_tty->line_cursor_pos = kbd_prev_char_pos(current_tty->line_cursor_pos);
            kbd_place_output_cursor(current_tty->line_cursor_pos);
            kbd_draw_cursor_at(current_tty->line_cursor_pos);
        }
        return;
    case 0x4D: //Стрелочка ->
        if (current_tty->line_cursor_pos < current_tty->copied_bytes)
        {
            kbd_erase_cursor_at(current_tty->line_cursor_pos);
            current_tty->line_cursor_pos = kbd_next_char_pos(current_tty->line_cursor_pos);
            kbd_place_output_cursor(current_tty->line_cursor_pos);
            kbd_draw_cursor_at(current_tty->line_cursor_pos);
        }
        return;
    case 0x47: //HOME
        if (current_tty->line_cursor_pos > 0)
        {
            kbd_erase_cursor_at(current_tty->line_cursor_pos);
            current_tty->line_cursor_pos = 0;
            kbd_place_output_cursor(0);
            kbd_draw_cursor_at(0);
        }
        return;
    case 0x4F: //END
        if (current_tty->line_cursor_pos < current_tty->copied_bytes)
        {
            kbd_erase_cursor_at(current_tty->line_cursor_pos);
            current_tty->line_cursor_pos = current_tty->copied_bytes;
            kbd_place_output_cursor(current_tty->line_cursor_pos);
            kbd_draw_cursor_at(current_tty->line_cursor_pos);
        }
        return;
    case 0x53: //DELETE — удаление символа правее курсора
        kbd_line_delete();
        return;
    case 0x0E: //BACKSPACE — удаление символа левее курсора
        kbd_line_backspace();
        return;
    default:
        break;
    }

    if (scancode < 128)
    {
        if (scancode == 0x3A)
        {
            caps_lock_active = !caps_lock_active;
            return;
        }
        if (scancode == 0x2A || scancode == 0x36)
        {
            shift_pressed = true;
            return;
        }
        if (scancode == 0x1C) //Enter
        {
            kbd_line_commit();
            return;
        }

        const locale_t *loc = locale_get_active();
        uint32_t cp = locale_resolve_key(loc, scancode, shift_pressed);
        if (cp != 0)
        {
            cp = kbd_apply_caps(cp, caps_lock_active);
            if (cp == '\n')
            {
                kbd_line_commit();
            }
            else if (cp == '\b')
            {
                kbd_line_backspace();
            }
            else if (cp >= 0x20 && cp != 0x7F)
            {
                kbd_line_insert_cp(cp);
            }
            //остальные control-символы (tab и пр.) в строку не попадают
        }
    }
    else
    {
        if (scancode == 0xAA || scancode == 0xB6)
        {
            shift_pressed = false;
        }
    }
}




void irq6_handler(struct regs *r)
{
    (void)r;
    //Фиксируем, что прерывание от дисковода успешно прилетело
    floppy_irq_fired = 1;
    //посылаем сигнал EOI (End of Interrupt) в контроллер PIC1 (порт 0x20)
    outb(0x20, 0x20);//io_wait();
}


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

    //Аппаратные прерывания (32–47)
    if (r->int_no >= 32 && r->int_no < 48)
    {
        //Обработка конкретных IRQ
        if (r->int_no == 32)
        {
            outb(0x20, 0x20);

            //Увеличиваем системные тики
            tick_count++;

            //Обновляем мигание курсора TTY
            kconsole_update_cursor();

            if ((tick_count & 5) == 0)
            {
                kernel_load_animation_next_step();
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
