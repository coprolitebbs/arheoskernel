#include "include-kernel/ports_io.h"

inline void outb(uint16_t port, uint8_t value){
    asm volatile ("outb %0, %1":: "a"(value), "Nd"(port));
}


inline uint8_t inb(uint16_t port){
    uint8_t value;
    asm volatile ("inb %1, %0":"=a"(value):"Nd"(port));
    return value;
}

inline void outw(uint16_t port, uint16_t value){
    asm volatile ("outw %0, %1"::"a"(value), "Nd"(port));
}

inline uint16_t inw(uint16_t port){
    uint16_t value;
    asm volatile ("inw %1, %0":"=a"(value):"Nd"(port));
    return value;
}

inline void io_wait(void){
    asm volatile ("outb %%al, $0x80"::"a"(0));
}

inline void insw(uint16_t port, void *addr, uint32_t count){
    asm volatile ("cld; rep insw":"+D"(addr),"+c"(count):"d" (port):"memory");
}

inline void outsw(uint16_t port, const void *addr, uint32_t count){
    asm volatile ("cld; rep outsw":"+S"(addr),"+c"(count):"d" (port));
}

inline void outl(uint16_t port, uint32_t value){
    asm volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline uint32_t inl(uint16_t port){
    uint32_t value;
    asm volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
