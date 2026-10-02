#ifndef ELF32_H
#define ELF32_H

#include <stdint.h>

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_NOBITS   8

// ELF relocation helpers

#define ELF32_R_SYM(i)   ((i) >> 8)
#define ELF32_R_TYPE(i)  ((uint8_t)(i))


#define R_386_NONE         0   //Нет релокации (игнорируется)
#define R_386_32           1   //Прямая 32-битная релокация (S + A)
#define R_386_PC32         2   //Относительная релокация к PC (S + A - P)
#define R_386_GOT32        3   //Адрес записи символа в GOT (G + A - P)
#define R_386_PLT32        4   //Адрес записи символа в PLT (L + A - P)
#define R_386_COPY         5   //Копирование символа рантаймом (используется для глобальных переменных из .so)
#define R_386_GLOB_DAT     6   //Установка адреса глобального символа в GOT (S)
#define R_386_JMP_SLOT     7   //Установка адреса функции в PLT/GOT (S)
#define R_386_RELATIVE     8   //Относительное смещение для PIE/shared-кода (B + A)
#define R_386_GOTOFF       9   //Смещение символа относительно базы GOT (S + A - GOT)
#define R_386_GOTPC        10  //Смещение самой GOT относительно PC (GOT + A - P)

//Расширенные типы для оптимизации линкера и Thread-Local Storage (TLS)
#define R_386_32PLT        11
#define R_386_TLS_GD       18  //Описание TLS в режиме Global Dynamic
#define R_386_TLS_LDM      19  //Описание TLS в режиме Local Dynamic
#define R_386_TLS_LDO_32   20
#define R_386_TLS_IE       21  //Описание TLS в режиме Initial Executable
#define R_386_TLS_LE       22  //Описание TLS в режиме Local Executable
#define R_386_TLS_IE_32    23
#define R_386_TLS_LE_32    24
#define R_386_TLS_GD_32    25
#define R_386_TLS_GD_PUSH  26
#define R_386_TLS_GD_CALL  27
#define R_386_TLS_GD_POP   28
#define R_386_TLS_LDM_32   29
#define R_386_TLS_LDM_PUSH 30
#define R_386_TLS_LDM_CALL 31
#define R_386_TLS_LDM_POP  32
#define R_386_TLS_LDO_STR  33
#define R_386_TLS_IE_STR   34
#define R_386_TLS_LE_STR   35
#define R_386_TLS_DTPMOD32 36
#define R_386_TLS_DTPOFF32 37
#define R_386_TLS_TPOFF32  38
#define R_386_IRELATIVE    43  //Непрямая относительная релокация (для функций типа GNU indirect functions)


typedef struct{
    unsigned char ident[16];

    uint16_t type;
    uint16_t machine;

    uint32_t version;

    uint32_t entry;

    uint32_t phoff;
    uint32_t shoff;

    uint32_t flags;

    uint16_t ehsize;

    uint16_t phentsize;
    uint16_t phnum;

    uint16_t shentsize;
    uint16_t shnum;

    uint16_t shstrndx;

} Elf32_Ehdr;



typedef struct
{
    uint32_t name;

    uint32_t type;

    uint32_t flags;

    uint32_t addr;

    uint32_t offset;

    uint32_t size;

    uint32_t link;

    uint32_t info;

    uint32_t addralign;

    uint32_t entsize;

} Elf32_Shdr;


typedef struct
{
    uint32_t offset;
    uint32_t info;

} Elf32_Rel;

typedef struct
{
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;

    uint8_t  st_info;
    uint8_t  st_other;

    uint16_t st_shndx;

} Elf32_Sym;

typedef struct {
    uint32_t type;     //Тип сегмента (например, PT_LOAD = 1)
    uint32_t offset;   //Смещение сегмента в файле на диске
    uint32_t vaddr;    //Виртуальный адрес сегмента в памяти процесса
    uint32_t paddr;    //Физический адрес (обычно игнорируется в x86)
    uint32_t filesz;   //Размер сегмента внутри дискового файла
    uint32_t memsz;    //Точный размер сегмента в ОЗУ (включая .bss)
    uint32_t flags;    //Флаги доступа (X | W | R)
    uint32_t align;    //Выравнивание сегмента в памяти (например, 4096)
} Elf32_Phdr;



#endif
