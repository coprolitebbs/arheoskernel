#include <stdint.h>

// Структура записи IDT (8 байт)
struct idt_entry {
    uint16_t base_low;   // младшие 16 бит адреса обработчика
    uint16_t sel;        // селектор сегмента кода (0x08)
    uint8_t  always0;    // должен быть 0
    uint8_t  flags;      // тип и атрибуты (0x8E для прерываний, 0xEE для ловушек)
    uint16_t base_high;  // старшие 16 бит адреса обработчика
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

// IDT на 256 записей
struct idt_entry idt[256];
struct idt_ptr idtp;

// Внешние функции-обработчики (определены в asm)
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
// ... isr0..isr31 для исключений
extern void irq0(void);  // таймер
extern void irq1(void);  // клавиатура
// ... irq0..irq15
extern void syscall_handler(void); // int 0x80

// Заполнение одной записи IDT
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

// Инициализация IDT
void idt_init(void) {
    // Заполняем IDT нулями
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // ---- Исключения (0-31) ----
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    // ... добавьте остальные isr3..isr31 по аналогии

    // ---- Аппаратные прерывания (IRQ) ----
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E); // таймер
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E); // клавиатура
    // ... irq2..irq15

    // ---- Системный вызов (int 0x80) ----
    idt_set_gate(0x80, (uint32_t)syscall_handler, 0x08, 0xEE); // ловушка (можно прерывание)

    // ---- Загружаем IDT ----
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    asm volatile("lidt (%0)" : : "r"(&idtp));
}

// Перенастройка PIC (для корректной работы IRQ)
void pic_init(void) {
    // Начинаем перенастройку
    asm volatile("mov $0x11, %al; out %al, $0x20; out %al, $0xA0");
    // Векторы: IRQ0..7 -> 0x20..0x27, IRQ8..15 -> 0x28..0x2F
    asm volatile("mov $0x20, %al; out %al, $0x21");
    asm volatile("mov $0x28, %al; out %al, $0xA1");
    // Настройка мастер- и слейв-контроллеров
    asm volatile("mov $0x04, %al; out %al, $0x21");
    asm volatile("mov $0x02, %al; out %al, $0xA1");
    asm volatile("mov $0x01, %al; out %al, $0x21");
    asm volatile("mov $0x01, %al; out %al, $0xA1");
    // Маскируем все прерывания (кроме тех, что нужны)
    asm volatile("mov $0xFF, %al; out %al, $0x21");
    asm volatile("mov $0xFF, %al; out %al, $0xA1");
}