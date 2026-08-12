#include "include-kernel/isr.h"
#include "include-kernel/syscalls.h"   // для SYS_* констант
#include "include-kernel/draw.h"       // для вызова draw_char из системного вызова
#include "include-kernel/task.h"
#include "include-kernel/lib.h"
#include "include-kernel/debug.h" 

int xoffs = 0;
int yoffs = 60;



// Внешние символы из isr.asm (обработчики)
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

extern void syscall_handler_asm(void); // int 0x80

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("out %0, %1" : : "a"(val), "Nd"(port));
}

// ---- Вспомогательная функция для заполнения IDT ----
static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

volatile uint32_t tick_count = 0;

// ---- Инициализация IDT ----
void idt_init(void) {
    //volatile unsigned short *video = (unsigned short*)0xB8000;
    //video[0] = (0x0F << 8) | 'I';  // выводим 'I' в верхнем левом углу

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // Исключения (0–31)
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
	
    // Аппаратные прерывания (32–47)
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);  // таймер
	idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);  // клавиатура
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


    // Системный вызов (int 0x80)
    idt_set_gate(0x80, (uint32_t)syscall_handler_asm, 0x08, 0xEE); // ловушка
	

    // Загружаем IDT
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    asm volatile("lidt (%0)" : : "r"(&idtp));
	//video[1] = (0x0F << 8) | 'L';
}

// ---- Перенастройка PIC ----
void pic_init(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    //outb(0x21, 0xFE);  // разрешить только IRQ0
	outb(0x21, 0xFC); // разрешить IRQ0 и IRQ1
    outb(0xA1, 0xFF);
}

void isr_halt13(struct regs *r){
	draw_string(10, 30, "GP: ");
	char buf[16];
	itoa(r->err_code, buf, 16);
	draw_string(120, 30, buf);
	draw_string(10, 40, "EIP: ");
	itoa(r->eip, buf, 16);
	draw_string(120, 40, buf);
	draw_string(10, 50, "CS: ");
	itoa(r->cs, buf, 16);
	draw_string(120, 50, buf);
	
	draw_string(10, 60, "Shedule count: ");
	itoa(shedule_count, buf, 16);
	draw_string(xoffs, yoffs, buf);
	//xoffs += 10;
	if(xoffs > 1020){
		xoffs = 0;
		yoffs += 10;
	}
	draw_string(10, 70, "user_esp: ");
	itoa(r->user_esp, buf, 16);
	draw_string(120, 70, buf);
	draw_string(10, 80, "user_eip: ");
	itoa(r->eip, buf, 16);
	draw_string(120, 80, buf);
	
	for(;;) {}
}


void isr_pagefault(struct regs *r){
	uint32_t cr2;
	asm volatile("mov %%cr2,%0":"=r"(cr2));

	draw_string(10,630,"PAGE FAULT");

	draw_string(10,640,"CR2:");
	draw_hex_dword(cr2,100,640);

	draw_string(10,650,"ERR:");
	draw_hex_dword(r->err_code,100,650);

	draw_string(10,660,"EIP:");
	draw_hex_dword(r->eip,100,660);

	draw_string(10,670,"CS:");
	draw_hex_dword(r->cs,100,670);

	draw_string(10,680,"ESP");
	draw_hex_dword(r->user_esp,100,680);
	
	
	uint32_t cr3;
	asm volatile("mov %%cr3,%0":"=r"(cr3));

	draw_string(10,690,"PF CR3:");
	draw_hex_dword(cr3,100,690);
	draw_hex_dword(current_task->page_dir_phys,250,690);

	uint32_t pdee = (*current_task->page_dir)[0x40000000>>22];
	draw_hex_dword(pdee,100,700);

	uint32_t *pt=(uint32_t*)(pdee & 0xFFFFF000);
	draw_hex_dword(pt[0],250,700);
	
	uint32_t va = r->eip;
	uint32_t pde_index = va >> 22;
	uint32_t pte_index = (va >> 12) & 0x3FF;
	draw_string(10,510,"PDE IDX:");
	draw_hex_dword(pde_index,100,510);
	draw_string(10,520,"PTE IDX:");
	draw_hex_dword(pte_index,100,520);

	uint32_t *dir = (uint32_t*)current_task->page_dir_phys;
	uint32_t pde = dir[pde_index];
	draw_string(10,530,"PDE:");
	draw_hex_dword(pde,100,530);


	if(pde & PAGE_PRESENT){
		uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000);
		uint32_t pte = pt[pte_index];
		draw_string(10,540,"PTE:");
		draw_hex_dword(pte,100,540);
	}

}





void isr_handler(struct regs *r) {
    // Исключения (0–31) — критическая ошибка
    if (r->int_no < 32) {
		if (r->int_no == 14) {
			//PAGE FAULT
			isr_pagefault(r);
			for(;;);
			
		}
		if (r->int_no == 13) {
			isr_halt13(r);
		}
	}
	
	if (r->int_no == 33) {
		draw_string(10, 520, "Keyboard IRQ!");
		outb(0x20, 0x20);
		return;
	}

    // Аппаратные прерывания (32–47)
    if (r->int_no >= 32 && r->int_no < 48) {
        // Обработка конкретных IRQ
        if (r->int_no == 32) {
            // ---- Таймер ----
            // Отправляем EOI мастеру
			/*if (current_task && current_task->pid != 0) {
				if(current_task->state == TASK_RUNNING) current_task->state = TASK_READY;
			}*/
            outb(0x20, 0x20);
            // Вызываем планировщик
			//draw_char(100, 100, 'D');
            schedule(r);
            return;   // важно: после schedule() управление не вернётся
        } else {
            // Для остальных IRQ — просто EOI
            if (r->int_no >= 40) {
                outb(0xA0, 0x20);
            }
            outb(0x20, 0x20);
        }
        return;
    }

    // Системный вызов (int 0x80)
    if (r->int_no == 0x80) {
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