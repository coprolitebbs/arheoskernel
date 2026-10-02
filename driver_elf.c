#include "include-kernel/driver_elf.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/elf32.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/vfs.h"
#include "include-kernel/kernel_heap.h"

#define DRIVER_LOAD_ADDR 0x250000

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

//Поиск секции по имени
static Elf32_Shdr *find_section(Elf32_Ehdr *ehdr, Elf32_Shdr *sections, char *shstrtab, const char *name){
    for (int i = 0; i < ehdr->shnum; i++){
        char *sname = shstrtab + sections[i].name;
        if (strcmp(sname, name) == 0)
            return &sections[i];
    }
    return NULL;
}

//Вычисление смещения символа в загруженном образе
static uint32_t symbol_offset(Elf32_Shdr *text, Elf32_Shdr *rodata, Elf32_Shdr *data, Elf32_Shdr *bss,
                              uint32_t text_off, uint32_t rodata_off, uint32_t data_off, uint32_t bss_off,
                              uint32_t addr){
    if (addr >= text->addr && addr < text->addr + text->size) return text_off + (addr - text->addr);
    if (rodata && addr >= rodata->addr && addr < rodata->addr + rodata->size) return rodata_off + (addr - rodata->addr);
    if (data && addr >= data->addr && addr < data->addr + data->size) return data_off + (addr - data->addr);
    if (bss && addr >= bss->addr && addr < bss->addr + bss->size) return bss_off + (addr - bss->addr);
    return 0xFFFFFFFF;
}




//Основная функция загрузки
void *load_driver_elf(unsigned char *elf_image,struct boot_info *boot){
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_image;
    //Проверка магии
    if (ehdr->ident[0] != 0x7F || ehdr->ident[1] != 'E' || ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F'){
        draw_string(10, 200, "BAD ELF MAGIC");
        return 0;
    }
    //Секции и строки
    Elf32_Shdr *sections = (Elf32_Shdr*)(elf_image + ehdr->shoff);
    char *shstrtab = (char*)(elf_image + sections[ehdr->shstrndx].offset);

    //Ищем нужные секции
    Elf32_Shdr *text = find_section(ehdr, sections, shstrtab, ".text");
    Elf32_Shdr *rodata = find_section(ehdr, sections, shstrtab, ".rodata");
    Elf32_Shdr *data = find_section(ehdr, sections, shstrtab, ".data");
    Elf32_Shdr *bss = find_section(ehdr, sections, shstrtab, ".bss");
    Elf32_Shdr *symtab = find_section(ehdr, sections, shstrtab, ".symtab");
    Elf32_Shdr *strtab = find_section(ehdr, sections, shstrtab, ".strtab");

    if (!text){
        draw_string(10, 220, "NO .text");
        return 0;
    }

    //Собираем все релокационные секции .rel.*
    Elf32_Shdr *rel_sections[20];
    int rel_count = 0;
    for (int i = 0; i < ehdr->shnum && rel_count < 20; i++){
        char *name = shstrtab + sections[i].name;
        if (strncmp(name, ".rel.", 5) == 0) rel_sections[rel_count++] = &sections[i];
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

    //Получаем таблицу символов
    Elf32_Sym *syms = NULL;
    char *strings = NULL;
    uint32_t sym_count = 0;
    if (symtab && strtab){
        syms = (Elf32_Sym*)(elf_image + symtab->offset);
        strings = (char*)(elf_image + strtab->offset);
        sym_count = symtab->size / sizeof(Elf32_Sym);
    }
    //Применяем релокации
    uint32_t main_offset = 0;
    uint32_t api_offset = 0;

    for (int r = 0; r < rel_count; r++){
        Elf32_Shdr *rel_sh = rel_sections[r];
        char *rel_name = shstrtab + rel_sh->name;
        uint32_t base_offset = 0;
        if (strstr(rel_name, ".text"))   base_offset = text_off;
        else if (strstr(rel_name, ".rodata")) base_offset = rodata_off;
        else if (strstr(rel_name, ".data"))   base_offset = data_off;
        else continue;

        Elf32_Rel *rels = (Elf32_Rel*)(elf_image + rel_sh->offset);
        int num = rel_sh->size / sizeof(Elf32_Rel);
        for (int i = 0; i < num; i++){
            uint32_t patch_addr = driver_start_load_addr + base_offset + rels[i].offset;
            uint32_t *patch = (uint32_t*)patch_addr;

            uint32_t sym_idx = ELF32_R_SYM(rels[i].info);
            uint32_t type = ELF32_R_TYPE(rels[i].info);

            if (sym_idx < sym_count && syms){
                char *sym_name = strings + syms[sym_idx].st_name;
                uint32_t sym_addr = syms[sym_idx].st_value;
                uint32_t sym_off = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, sym_addr);
                if (sym_off == 0xFFFFFFFF) continue;
                uint32_t sym_load_addr = driver_start_load_addr + sym_off;

                if (type == R_386_32){ // R_386_32
                    *patch = sym_load_addr;
                } else if (type == 2) { // R_386_PC32
                    uint32_t reloc_addr = driver_start_load_addr + base_offset + rels[i].offset;
                    *patch = sym_load_addr - reloc_addr - 4;
                }
                // Если это точка входа – запоминаем
                if (strcmp(sym_name, "driver_main") == 0) main_offset = sym_off;
                if(strcmp(sym_name,"driver_get_api") == 0) api_offset = sym_off;
            }
        }
    }

    //Если точка входа не найдена – ищем её отдельно
    if (main_offset == 0 && syms){
        for (uint32_t i = 0; i < sym_count; i++){
            char *name = strings + syms[i].st_name;
            if (strcmp(name, "driver_main") == 0){
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

    if (main_offset == 0){
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

//Основная функция загрузки из памяти
void *load_driver_elf_mem(uint32_t addr,uint32_t size,struct boot_info *boot){
    unsigned char *elf_image=(unsigned char*)addr;
    Elf32_Ehdr *ehdr=(Elf32_Ehdr*)elf_image;

    //Проверка магии
    if (ehdr->ident[0] != 0x7F || ehdr->ident[1] != 'E' || ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F'){
        draw_string(10, 200, "BAD ELF MAGIC");
        return 0;
    }

    //Секции и строки
    Elf32_Shdr *sections = (Elf32_Shdr*)(elf_image + ehdr->shoff);
    char *shstrtab = (char*)(elf_image + sections[ehdr->shstrndx].offset);

    // Ищем нужные секции
    Elf32_Shdr *text = find_section(ehdr, sections, shstrtab, ".text");
    Elf32_Shdr *rodata = find_section(ehdr, sections, shstrtab, ".rodata");
    Elf32_Shdr *data = find_section(ehdr, sections, shstrtab, ".data");
    Elf32_Shdr *bss = find_section(ehdr, sections, shstrtab, ".bss");
    Elf32_Shdr *symtab = find_section(ehdr, sections, shstrtab, ".symtab");
    Elf32_Shdr *strtab = find_section(ehdr, sections, shstrtab, ".strtab");

    if (!text){
        draw_string(10, 220, "NO .text");
        return 0;
    }

    // Собираем все релокационные секции .rel.*
    Elf32_Shdr *rel_sections[20];
    int rel_count = 0;
    for (int i = 0; i < ehdr->shnum && rel_count < 20; i++){
        char *name = shstrtab + sections[i].name;
        if (strncmp(name, ".rel.", 5) == 0) rel_sections[rel_count++] = &sections[i];
    }

    // Вычисляем смещения в загруженной памяти (как в driver_builder)
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
    //Копируем секции в целевую область
    uint8_t *dst = (uint8_t*)driver_start_load_addr;

    memcpy(dst + text_off, elf_image + text->offset, text->size);
    if (rodata) memcpy(dst + rodata_off, elf_image + rodata->offset, rodata->size);
    if (data)   memcpy(dst + data_off,   elf_image + data->offset,   data->size);
    if (bss)    memset(dst + bss_off, 0, bss->size);

    //Получаем таблицу символов
    Elf32_Sym *syms = NULL;
    char *strings = NULL;
    uint32_t sym_count = 0;
    if (symtab && strtab){
        syms = (Elf32_Sym*)(elf_image + symtab->offset);
        strings = (char*)(elf_image + strtab->offset);
        sym_count = symtab->size / sizeof(Elf32_Sym);
    }

    //Применяем релокации
    uint32_t main_offset = 0;
    uint32_t api_offset = 0;

    for (int r = 0; r < rel_count; r++){
        Elf32_Shdr *rel_sh = rel_sections[r];
        char *rel_name = shstrtab + rel_sh->name;
        uint32_t base_offset = 0;
        if (strstr(rel_name, ".text"))   base_offset = text_off;
        else if (strstr(rel_name, ".rodata")) base_offset = rodata_off;
        else if (strstr(rel_name, ".data")) base_offset = data_off;
        else if (strstr(rel_name, ".bss")) base_offset = bss_off;
        else continue;

        Elf32_Rel *rels = (Elf32_Rel*)(elf_image + rel_sh->offset);
        int num = rel_sh->size / sizeof(Elf32_Rel);
        for (int i = 0; i < num; i++){
            uint32_t patch_addr = driver_start_load_addr + base_offset + rels[i].offset;
            uint32_t *patch = (uint32_t*)patch_addr;

            uint32_t sym_idx = ELF32_R_SYM(rels[i].info);
            uint32_t type = ELF32_R_TYPE(rels[i].info);

            if (sym_idx < sym_count && syms){
                char *sym_name = strings + syms[sym_idx].st_name;
                uint32_t sym_addr = syms[sym_idx].st_value;
                uint32_t sym_off = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, sym_addr);
                if (sym_off == 0xFFFFFFFF) continue;
                uint32_t sym_load_addr = driver_start_load_addr + sym_off;

                if (type == R_386_32){ // R_386_32
                    *patch = sym_load_addr;
                } else if (type == 2){ // R_386_PC32
                    uint32_t reloc_addr = driver_start_load_addr + base_offset + rels[i].offset;
                    *patch = sym_load_addr - reloc_addr - 4;
                }

                //Если это точка входа – запоминаем
                if (strcmp(sym_name, "driver_main") == 0) main_offset = sym_off;
                if(strcmp(sym_name,"driver_get_api")==0) api_offset = sym_off;
            }

        }
    }

    //Если точка входа не найдена – ищем её отдельно
    if (main_offset == 0 && syms){
        for (uint32_t i = 0; i < sym_count; i++){
            char *name = strings + syms[i].st_name;
            if (strcmp(name, "driver_main") == 0){
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

    if (main_offset == 0){
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


void* load_driver_elf_from_disk(const char *path, struct boot_info *boot){
    if (!path || !boot) return NULL;
    fs_stat_t st;
    memset(&st, 0, sizeof(fs_stat_t));

    int stat_res = vfs_stat(path, &st);
    if (stat_res != FS_OK){
        com_puts("[KERNEL ERROR] load_driver: Cannot stat driver file: ");
        com_puts(path);
        com_puts("\n");
        return NULL;
    }

    uint32_t file_size = st.size;

    //Защитная проверка лимитов на размер ELF-модуля
    if (file_size == 0 || file_size > 512 * 1024){
        com_puts("[KERNEL ERROR] load_driver: Invalid ELF file size: ");
        com_puthex(file_size);
        com_puts("\n");
        return NULL;
    }

    //Открываем файл драйвера через конкретный драйвер в режиме чтения
    int fd = vfs_open(path, FS_OPEN_READ);
    if (fd < 0){
        com_puts("[KERNEL ERROR] load_driver: Cannot open file: ");
        com_puts(path);
        com_puts("\n");
        return NULL;
    }

    //Выделяем временный буфер в куче ядра под сырой образ ELF
    uint8_t *elf_image = (uint8_t *)kmalloc(file_size);
    if (!elf_image) {
        vfs_close(fd);
        com_puts("[KERNEL ERROR] load_driver: Out of memory for ELF buffer\n");
        return NULL;
    }
    memset(elf_image, 0, file_size);

    //Посекторно вычитываем весь файл с диска в кучу ядра (порциями по 512 байт)
    int bytes_read = 0;
    while (bytes_read < file_size){
        int chunk = file_size - bytes_read;
        if (chunk > 512) chunk = 512;

        int r = vfs_read(fd, elf_image + bytes_read, chunk);
        if (r <= 0) break; //Конец файла или аппаратная ошибка
        bytes_read += r;
    }
    vfs_close(fd); //Файл на диске больше не нужен, закрываем дескриптор

    if (bytes_read < file_size){
        kfree(elf_image);
        com_puts("[KERNEL ERROR] load_driver: Read error or truncated file\n");
        return NULL;
    }

    //Парсинг и релокация вычитанного ELF-образа
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_image;

    //Проверяем сигнатуру ELF-magic
    if (ehdr->ident[0] != 0x7F || ehdr->ident[1] != 'E' || ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F'){
        kfree(elf_image);
        draw_string(10, 200, "BAD ELF MAGIC ON DISK");
        return NULL;
    }

    Elf32_Shdr *sections = (Elf32_Shdr*)(elf_image + ehdr->shoff);
    char *shstrtab = (char*)(elf_image + sections[ehdr->shstrndx].offset);

    //Локализуем ключевые секции бинарника
    Elf32_Shdr *text = find_section(ehdr, sections, shstrtab, ".text");
    Elf32_Shdr *rodata = find_section(ehdr, sections, shstrtab, ".rodata");
    Elf32_Shdr *data = find_section(ehdr, sections, shstrtab, ".data");
    Elf32_Shdr *bss = find_section(ehdr, sections, shstrtab, ".bss");
    Elf32_Shdr *symtab = find_section(ehdr, sections, shstrtab, ".symtab");
    Elf32_Shdr *strtab = find_section(ehdr, sections, shstrtab, ".strtab");

    if (!text){
        kfree(elf_image);
        draw_string(10, 220, "NO .text SECTION");
        return NULL;
    }

    //Собираем таблицы релокаций .rel.* (до 20 штук)
    Elf32_Shdr *rel_sections[20];
    int rel_count = 0;
    for (int i = 0; i < ehdr->shnum && rel_count < 20; i++){
        char *name = shstrtab + sections[i].name;
        if (strncmp(name, ".rel.", 5) == 0){
            rel_sections[rel_count++] = &sections[i];
        }
    }
    //Расчитываем когерентные смещения секций в памяти загрузки
    int32_t offset = 0;
    uint32_t text_off = align_up(offset, text->addralign);
    offset = text_off + text->size;

    uint32_t rodata_off = 0;
    if (rodata){
        rodata_off = align_up(offset, rodata->addralign);
        offset = rodata_off + rodata->size;
    }

    uint32_t data_off = 0;
    if (data){
        data_off = align_up(offset, data->addralign);
        offset = data_off + data->size;
    }

    uint32_t bss_off = 0;
    if (bss){
        bss_off = align_up(offset, bss->addralign);
        offset = bss_off + bss->size;
    }

    //Копируем чистый машинный код и данные в глобальную целевую область драйверов Ring 0
    uint8_t *dst = (uint8_t*)driver_start_load_addr;
    memcpy(dst + text_off, elf_image + text->offset, text->size);
    if (rodata) memcpy(dst + rodata_off, elf_image + rodata->offset, rodata->size);
    if (data) memcpy(dst + data_off,   elf_image + data->offset,   data->size);
    if (bss) memset(dst + bss_off, 0, bss->size);

    // Извлекаем таблицы символов и строк
    Elf32_Sym *syms = NULL;
    char *strings = NULL;
    uint32_t sym_count = 0;
    if (symtab && strtab){
        syms = (Elf32_Sym*)(elf_image + symtab->offset);
        strings = (char*)(elf_image + strtab->offset);
        sym_count = symtab->size / sizeof(Elf32_Sym);
    }

    // Применяем релокации R_386_32 и R_386_PC32
    uint32_t main_offset = 0;
    uint32_t api_offset = 0;

    for (int r_sec = 0; r_sec < rel_count; r_sec++){
        Elf32_Shdr *rel_sh = rel_sections[r_sec];
        char *rel_name = shstrtab + rel_sh->name;
        uint32_t base_offset = 0;

        if (strstr(rel_name, ".text"))        base_offset = text_off;
        else if (strstr(rel_name, ".rodata")) base_offset = rodata_off;
        else if (strstr(rel_name, ".data"))   base_offset = data_off;
        else if (strstr(rel_name, ".bss"))    base_offset = bss_off;
        else continue;

        Elf32_Rel *rels = (Elf32_Rel*)(elf_image + rel_sh->offset);
        int num = rel_sh->size / sizeof(Elf32_Rel);

        for (int i = 0; i < num; i++) {
            uint32_t patch_addr = driver_start_load_addr + base_offset + rels[i].offset;
            uint32_t *patch = (uint32_t*)patch_addr;

            uint32_t sym_idx = ELF32_R_SYM(rels[i].info);
            uint32_t type = ELF32_R_TYPE(rels[i].info);

            if (sym_idx < sym_count && syms){
                char *sym_name = strings + syms[sym_idx].st_name;
                uint32_t sym_addr = syms[sym_idx].st_value;
                uint32_t sym_off = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, sym_addr);
                if (sym_off == 0xFFFFFFFF) continue;

                uint32_t sym_load_addr = driver_start_load_addr + sym_off;

                if (type == 1){ // R_386_32
                    *patch = sym_load_addr;
                } else if (type == 2){ // R_386_PC32
                    uint32_t reloc_addr = driver_start_load_addr + base_offset + rels[i].offset;
                    *patch = sym_load_addr - reloc_addr - 4;
                }

                if (strcmp(sym_name, "driver_main") == 0)    main_offset = sym_off;
                if (strcmp(sym_name, "driver_get_api") == 0) api_offset = sym_off;
            }
        }
    }

    // Страховочный поиск точек экспорта, если релокации их пропустили
    if (main_offset == 0 && syms){
        for (uint32_t i = 0; i < sym_count; i++){
            if (strcmp(strings + syms[i].st_name, "driver_main") == 0){
                main_offset = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, syms[i].st_value);
                break;
            }
        }
    }
    if (api_offset == 0 && syms){
        for (uint32_t i = 0; i < sym_count; i++){
            if (strcmp(strings + syms[i].st_name, "driver_get_api") == 0){
                api_offset = symbol_offset(text, rodata, data, bss, text_off, rodata_off, data_off, bss_off, syms[i].st_value);
                break;
            }
        }
    }

    if (main_offset == 0 || api_offset == 0){
        kfree(elf_image);
        draw_string(10, 240, "NO DRIVER ENTRY OR API EXPORT");
        return NULL;
    }

    // Инициализируем указатели на функции внутри загруженного модуля
    void (*driver_main)(struct boot_info*) = (void*)(driver_start_load_addr + main_offset);
    void *(*get_api)(void) = (void *(*)(void))(driver_start_load_addr + api_offset);
    // Вызываем главную функцию драйвера для регистрации внутренних структур
    driver_main(boot);
    // Забираем заполненную структуру API драйвера
    void *api_return_ptr = get_api();
    kfree(elf_image);

    if (api_return_ptr){
        // Сдвигаем базовый адрес загрузки следующего драйвера вперед на размер текущего модуля
        driver_start_load_addr += align_up(offset, 0x1000);
    }

    return api_return_ptr;
}

