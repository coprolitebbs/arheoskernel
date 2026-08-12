#ifndef ELF32_H
#define ELF32_H

#include <stdint.h>


typedef struct
{
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

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_NOBITS   8


// ------------------------------------------------------------
// ELF relocation helpers
// ------------------------------------------------------------

#define ELF32_R_SYM(i)   ((i) >> 8)
#define ELF32_R_TYPE(i)  ((uint8_t)(i))


#define R_386_NONE      0
#define R_386_32        1
#define R_386_PC32      2


#endif
