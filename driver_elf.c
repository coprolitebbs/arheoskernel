#include "include-kernel/driver_elf.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/elf32.h"
#include "include-kernel/bootinfo.h"
//#include "drivers/include_drivers/drv_format.h"

#define DRIVER_LOAD_ADDR 0x250000

// Используем макросы из elf32.h (если они уже определены, не переопределяем)
#ifndef ELF32_R_SYM
#define ELF32_R_SYM(i)   ((i) >> 8)
#endif
#ifndef ELF32_R_TYPE
#define ELF32_R_TYPE(i)  ((uint8_t)(i))
#endif

uint32_t driver_start_load_addr = DRIVER_LOAD_ADDR;


static uint32_t align_up(uint32_t value,uint32_t align){
    return (value + align - 1) & ~(align - 1);
}


// Поиск секции по имени
static Elf32_Shdr *find_section(Elf32_Ehdr *ehdr, Elf32_Shdr *sections, char *shstrtab, const char *name) {
    for (int i = 0; i < ehdr->shnum; i++) {
        char *sname = shstrtab + sections[i].name;
        if (strcmp(sname, name) == 0)
            return &sections[i];
    }
    return NULL;
}

// Вычисление смещения символа в загруженном образе
static uint32_t symbol_offset(Elf32_Shdr *text, Elf32_Shdr *rodata, Elf32_Shdr *data, Elf32_Shdr *bss,
                              uint32_t text_off, uint32_t rodata_off, uint32_t data_off, uint32_t bss_off,
                              uint32_t addr) {
    if (addr >= text->addr && addr < text->addr + text->size)
        return text_off + (addr - text->addr);
    if (rodata && addr >= rodata->addr && addr < rodata->addr + rodata->size)
        return rodata_off + (addr - rodata->addr);
    if (data && addr >= data->addr && addr < data->addr + data->size)
        return data_off + (addr - data->addr);
    if (bss && addr >= bss->addr && addr < bss->addr + bss->size)
        return bss_off + (addr - bss->addr);
    return 0xFFFFFFFF;
}




// Основная функция загрузки
//void *load_driver_elf(unsigned char *elf_image)
void *load_driver_elf(unsigned char *elf_image,struct boot_info *boot)
{
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_image;

    // Проверка магии
    if (ehdr->ident[0] != 0x7F || ehdr->ident[1] != 'E' || ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F') {
        draw_string(10, 200, "BAD ELF MAGIC");
        return 0;
    }

    // Секции и строки
    Elf32_Shdr *sections = (Elf32_Shdr*)(elf_image + ehdr->shoff);
    char *shstrtab = (char*)(elf_image + sections[ehdr->shstrndx].offset);

    // Ищем нужные секции
    Elf32_Shdr *text = find_section(ehdr, sections, shstrtab, ".text");
    Elf32_Shdr *rodata = find_section(ehdr, sections, shstrtab, ".rodata");
    Elf32_Shdr *data = find_section(ehdr, sections, shstrtab, ".data");
    Elf32_Shdr *bss = find_section(ehdr, sections, shstrtab, ".bss");
    Elf32_Shdr *symtab = find_section(ehdr, sections, shstrtab, ".symtab");
    Elf32_Shdr *strtab = find_section(ehdr, sections, shstrtab, ".strtab");

    if (!text) {
        draw_string(10, 220, "NO .text");
        return 0;
    }

    // Собираем все релокационные секции .rel.*
    Elf32_Shdr *rel_sections[20];
    int rel_count = 0;
    for (int i = 0; i < ehdr->shnum && rel_count < 20; i++) {
        char *name = shstrtab + sections[i].name;
        if (strncmp(name, ".rel.", 5) == 0)
            rel_sections[rel_count++] = &sections[i];
    }

    int32_t offset = 0;


    uint32_t text_off = align_up(offset,text->addralign);

    offset = text_off + text->size;

    uint32_t rodata_off = 0;

    if(rodata){
        rodata_off = align_up(offset,rodata->addralign);

        offset = rodata_off + rodata->size;
    }

    uint32_t data_off = 0;

    if(data){
        data_off = align_up(offset,data->addralign);

        offset = data_off + data->size;
    }

    uint32_t bss_off = 0;

    if(bss){
        bss_off = align_up(offset,bss->addralign);

        offset = bss_off + bss->size;
    }

    // Копируем секции в целевую область
    uint8_t *dst = (uint8_t*)driver_start_load_addr;

    memcpy(dst + text_off, elf_image + text->offset, text->size);
    if (rodata) memcpy(dst + rodata_off, elf_image + rodata->offset, rodata->size);
    if (data)   memcpy(dst + data_off,   elf_image + data->offset,   data->size);
    if (bss)    memset(dst + bss_off, 0, bss->size);

    // Получаем таблицу символов
    Elf32_Sym *syms = NULL;
    char *strings = NULL;
    uint32_t sym_count = 0;
    if (symtab && strtab) {
        syms = (Elf32_Sym*)(elf_image + symtab->offset);
        strings = (char*)(elf_image + strtab->offset);
        sym_count = symtab->size / sizeof(Elf32_Sym);
    }

    // Применяем релокации
    //uint32_t entry_offset = 0;
    uint32_t main_offset = 0;
    uint32_t api_offset = 0;

    for (int r = 0; r < rel_count; r++) {
        Elf32_Shdr *rel_sh = rel_sections[r];
        char *rel_name = shstrtab + rel_sh->name;
        uint32_t base_offset = 0;
        if (strstr(rel_name, ".text"))   base_offset = text_off;
        else if (strstr(rel_name, ".rodata")) base_offset = rodata_off;
        else if (strstr(rel_name, ".data"))   base_offset = data_off;
        else continue;

        Elf32_Rel *rels = (Elf32_Rel*)(elf_image + rel_sh->offset);
        int num = rel_sh->size / sizeof(Elf32_Rel);
        for (int i = 0; i < num; i++) {
            uint32_t patch_addr = driver_start_load_addr + base_offset + rels[i].offset;
            uint32_t *patch = (uint32_t*)patch_addr;

            uint32_t sym_idx = ELF32_R_SYM(rels[i].info);
            uint32_t type = ELF32_R_TYPE(rels[i].info);

            if (sym_idx < sym_count && syms) {
                char *sym_name = strings + syms[sym_idx].st_name;
                uint32_t sym_addr = syms[sym_idx].st_value;
                uint32_t sym_off = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, sym_addr);
                if (sym_off == 0xFFFFFFFF) continue;
                uint32_t sym_load_addr = driver_start_load_addr + sym_off;


//static int dbg_y = 300;

                if (type == R_386_32) { // R_386_32
                    *patch = sym_load_addr;
                } else if (type == 2) { // R_386_PC32
                    uint32_t reloc_addr = driver_start_load_addr + base_offset + rels[i].offset;
                    *patch = sym_load_addr - reloc_addr - 4;
                }

                // Если это точка входа – запоминаем
                if (strcmp(sym_name, "driver_main") == 0) {
                    main_offset = sym_off;
                }
                if(strcmp(sym_name,"driver_get_api")==0)
                    api_offset = sym_off;
            }

        }
    }

    // Если точка входа не найдена – ищем её отдельно
    if (main_offset == 0 && syms) {
        for (uint32_t i = 0; i < sym_count; i++) {
            char *name = strings + syms[i].st_name;
            if (strcmp(name, "driver_main") == 0) {
                uint32_t sym_addr = syms[i].st_value;
                main_offset = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, sym_addr);
                break;
            }
        }
    }

    if(api_offset == 0 && syms){
        for(uint32_t i=0;i<sym_count;i++){
            char *name = strings + syms[i].st_name;
            if(strcmp(name,"driver_get_api")==0){
                uint32_t sym_addr = syms[i].st_value;
                api_offset = symbol_offset(
                    text,
                    rodata,
                    data,
                    bss,
                    text_off,
                    rodata_off,
                    data_off,
                    bss_off,
                    sym_addr
                );
                break;
            }
        }
    }


    if (main_offset == 0) {
        draw_string(10, 240, "NO ENTRY");
        return 0;
    }

    if(api_offset == 0){
        draw_string(10,280,"NO API");
        return 0;
    }

    void (*driver_main)(struct boot_info*) = (void*)(driver_start_load_addr + main_offset);

    void *(*get_api)(void);
    get_api = (void *(*)(void))(driver_start_load_addr + api_offset);
    driver_main = (void (*)(struct boot_info*))(driver_start_load_addr + main_offset);
    driver_main(boot_info);

    void *p = get_api();

    if(p){
        driver_start_load_addr += align_up(offset,0x1000);
    }

    return p;
}










// Основная функция загрузки из памяти
void *load_driver_elf_mem(uint32_t addr,uint32_t size,struct boot_info *boot)
{
    unsigned char *elf_image=(unsigned char*)addr;
    Elf32_Ehdr *ehdr=(Elf32_Ehdr*)elf_image;

    // Проверка магии
    if (ehdr->ident[0] != 0x7F || ehdr->ident[1] != 'E' || ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F') {
        draw_string(10, 200, "BAD ELF MAGIC");
        return 0;
    }

    // Секции и строки
    Elf32_Shdr *sections = (Elf32_Shdr*)(elf_image + ehdr->shoff);
    char *shstrtab = (char*)(elf_image + sections[ehdr->shstrndx].offset);

    // Ищем нужные секции
    Elf32_Shdr *text = find_section(ehdr, sections, shstrtab, ".text");
    Elf32_Shdr *rodata = find_section(ehdr, sections, shstrtab, ".rodata");
    Elf32_Shdr *data = find_section(ehdr, sections, shstrtab, ".data");
    Elf32_Shdr *bss = find_section(ehdr, sections, shstrtab, ".bss");
    Elf32_Shdr *symtab = find_section(ehdr, sections, shstrtab, ".symtab");
    Elf32_Shdr *strtab = find_section(ehdr, sections, shstrtab, ".strtab");

    if (!text) {
        draw_string(10, 220, "NO .text");
        return 0;
    }

    // Собираем все релокационные секции .rel.*
    Elf32_Shdr *rel_sections[20];
    int rel_count = 0;
    for (int i = 0; i < ehdr->shnum && rel_count < 20; i++) {
        char *name = shstrtab + sections[i].name;
        if (strncmp(name, ".rel.", 5) == 0)
            rel_sections[rel_count++] = &sections[i];
    }

    // Вычисляем смещения в загруженной памяти (как в driver_builder)
    int32_t offset = 0;


uint32_t text_off =
    align_up(
        offset,
        text->addralign
    );

offset =
    text_off +
    text->size;



uint32_t rodata_off = 0;

if(rodata)
{
    rodata_off =
        align_up(
            offset,
            rodata->addralign
        );

    offset =
        rodata_off +
        rodata->size;
}



uint32_t data_off = 0;

if(data)
{
    data_off =
        align_up(
            offset,
            data->addralign
        );

    offset =
        data_off +
        data->size;
}

uint32_t bss_off = 0;

if(bss)
{
    bss_off =
        align_up(
            offset,
            bss->addralign
        );

    offset =
        bss_off +
        bss->size;
}
    // Копируем секции в целевую область
    uint8_t *dst = (uint8_t*)driver_start_load_addr;

    memcpy(dst + text_off, elf_image + text->offset, text->size);
    if (rodata) memcpy(dst + rodata_off, elf_image + rodata->offset, rodata->size);
    if (data)   memcpy(dst + data_off,   elf_image + data->offset,   data->size);
    if (bss)    memset(dst + bss_off, 0, bss->size);

    // Получаем таблицу символов
    Elf32_Sym *syms = NULL;
    char *strings = NULL;
    uint32_t sym_count = 0;
    if (symtab && strtab) {
        syms = (Elf32_Sym*)(elf_image + symtab->offset);
        strings = (char*)(elf_image + strtab->offset);
        sym_count = symtab->size / sizeof(Elf32_Sym);
    }

    // Применяем релокации
    uint32_t main_offset = 0;
    uint32_t api_offset = 0;

    for (int r = 0; r < rel_count; r++) {
        Elf32_Shdr *rel_sh = rel_sections[r];
        char *rel_name = shstrtab + rel_sh->name;
        uint32_t base_offset = 0;
        if (strstr(rel_name, ".text"))   base_offset = text_off;
        else if (strstr(rel_name, ".rodata")) base_offset = rodata_off;
        else if (strstr(rel_name, ".data"))   base_offset = data_off;
        else continue;

        Elf32_Rel *rels = (Elf32_Rel*)(elf_image + rel_sh->offset);
        int num = rel_sh->size / sizeof(Elf32_Rel);
        for (int i = 0; i < num; i++) {
            uint32_t patch_addr = driver_start_load_addr + base_offset + rels[i].offset;
            uint32_t *patch = (uint32_t*)patch_addr;

            uint32_t sym_idx = ELF32_R_SYM(rels[i].info);
            uint32_t type = ELF32_R_TYPE(rels[i].info);

            if (sym_idx < sym_count && syms) {
                char *sym_name = strings + syms[sym_idx].st_name;
                uint32_t sym_addr = syms[sym_idx].st_value;
                uint32_t sym_off = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, sym_addr);
                if (sym_off == 0xFFFFFFFF) continue;
                uint32_t sym_load_addr = driver_start_load_addr + sym_off;

                if (type == R_386_32) { // R_386_32
                    *patch = sym_load_addr;
                } else if (type == 2) { // R_386_PC32
                    uint32_t reloc_addr = driver_start_load_addr + base_offset + rels[i].offset;
                    *patch = sym_load_addr - reloc_addr - 4;
                }

                // Если это точка входа – запоминаем
                if (strcmp(sym_name, "driver_main") == 0) {
                    main_offset = sym_off;
                }
                if(strcmp(sym_name,"driver_get_api")==0)
                    api_offset = sym_off;
            }

        }
    }

    // Если точка входа не найдена – ищем её отдельно
    if (main_offset == 0 && syms) {
        for (uint32_t i = 0; i < sym_count; i++) {
            char *name = strings + syms[i].st_name;
            if (strcmp(name, "driver_main") == 0) {
                uint32_t sym_addr = syms[i].st_value;
                main_offset = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, sym_addr);
                break;
            }
        }
    }

    if(api_offset == 0 && syms){
        for(uint32_t i=0;i<sym_count;i++){
            char *name = strings + syms[i].st_name;
            if(strcmp(name,"driver_get_api")==0){
                uint32_t sym_addr = syms[i].st_value;
                api_offset = symbol_offset(
                    text,
                    rodata,
                    data,
                    bss,
                    text_off,
                    rodata_off,
                    data_off,
                    bss_off,
                    sym_addr
                );
                break;
            }
        }
    }


    if (main_offset == 0) {
        draw_string(10, 240, "NO ENTRY");
        return 0;
    }

    if(api_offset == 0){
        draw_string(10,280,"NO API");
        return 0;
    }

    void (*driver_main)(struct boot_info*) = (void*)(driver_start_load_addr + main_offset);

    fb_driver_api_t *(*get_api)(void);
    get_api = (fb_driver_api_t *(*)(void))(driver_start_load_addr + api_offset);
    driver_main = (void (*)(struct boot_info*))(driver_start_load_addr + main_offset);
    driver_main(boot_info);


    fb_driver_api_t *p = get_api();

    if(p) driver_start_load_addr += align_up(offset,0x1000);

    return p;

}



