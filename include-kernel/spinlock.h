#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

//Структура спинлока ядра
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

//Инициализация замка (0 - свободен)
static inline void spin_lock_init(spinlock_t *lock){
    lock->lock = 0;
}

//Захват спинлока (атомарное ожидание)
static inline void spin_lock(spinlock_t *lock) {
    uint32_t val = 1;
    while (val == 1) {
        __asm__ __volatile__(
            "lock xchgl %0, %1"
            : "+r"(val), "+m"(lock->lock)
            :: "memory", "cc"
        );
        if (val == 1) {
            __asm__ __volatile__("pause");
            val = 1;
        }
    }
}

//Освобождение спинлока (атомарный сброс в 0)
static inline void spin_unlock(spinlock_t *lock) {
    uint32_t val = 0;
    __asm__ __volatile__(
        "lock xchgl %0, %1"
        : "+r"(val), "+m"(lock->lock)
        :: "memory", "cc"
    );
}

static inline uint32_t spin_lock_irqsave(spinlock_t *lock) {
    uint32_t eflags;
    //Инструкция pushf заталкивает EFLAGS на стек, pop вытаскивает в переменную
    //После этого безопасно тушим прерывания через cli
    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(eflags)
        :: "memory"
    );

    spin_lock(lock);
    return eflags; //Возвращаем старое состояние флагов процессора
}

//Освобождает спинлок и включает прерывания процессора обратно
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint32_t eflags) {
    spin_unlock(lock);
    //Проверяем, был ли взведен 9-й бит (Interrupt Flag) в сохраненных флагах
    //Включаем прерывания (sti) только если они реально были включены до этого
    if (eflags & 0x200) {
        __asm__ __volatile__("sti" ::: "memory");
    }
}

#endif

