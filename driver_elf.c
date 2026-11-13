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

#include "include-kernel/driver_elf.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/elf32.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/vfs.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/kconsole.h"

//#define DRIVER_LOAD_ADDR 0x250000
#define DRIVER_LOAD_ADDR 0xC00000

#ifndef ELF32_R_SYM
#define ELF32_R_SYM(i)   ((i) >> 8)
#endif
#ifndef ELF32_R_TYPE
#define ELF32_R_TYPE(i)  ((uint8_t)(i))
#endif

uint32_t driver_start_load_addr = DRIVER_LOAD_ADDR;

static uint32_t align_up(uint32_t value,uint32_t align)
{
    return (value + align - 1) & ~(align - 1);
}

//Поиск секции по имени
static Elf32_Shdr *find_section(Elf32_Ehdr *ehdr, Elf32_Shdr *sections, char *shstrtab, const char *name)
{
    for (int i = 0; i < ehdr->shnum; i++)
    {
        char *sname = shstrtab + sections[i].name;
        if (strcmp(sname, name) == 0)
            return &sections[i];
    }
    return NULL;
}

//Вычисление смещения символа в загруженном образе
static uint32_t symbol_offset(Elf32_Shdr *text, Elf32_Shdr *rodata, Elf32_Shdr *data, Elf32_Shdr *bss,
                              uint32_t text_off, uint32_t rodata_off, uint32_t data_off, uint32_t bss_off,
                              uint32_t addr)
{
    if (addr >= text->addr && addr < text->addr + text->size) return text_off + (addr - text->addr);
    if (rodata && addr >= rodata->addr && addr < rodata->addr + rodata->size) return rodata_off + (addr - rodata->addr);
    if (data && addr >= data->addr && addr < data->addr + data->size) return data_off + (addr - data->addr);
    if (bss && addr >= bss->addr && addr < bss->addr + bss->size) return bss_off + (addr - bss->addr);
    return 0xFFFFFFFF;
}


//Узнать, куда в загруженном модуле попала секция с заданным shndx
static uint32_t section_base_for_shndx(Elf32_Shdr *sections, uint32_t shndx,
                                       const char *shstrtab,
                                       uint32_t text_off, uint32_t rodata_off,
                                       uint32_t data_off, uint32_t bss_off)
{
    if (shndx == 0 || shndx >= 0xFF00) return 0xFFFFFFFF;
    const char *name = shstrtab + sections[shndx].name;
    if (strcmp(name, ".text")   == 0) return text_off;
    if (strcmp(name, ".rodata") == 0) return rodata_off;
    if (strcmp(name, ".data")   == 0) return data_off;
    if (strcmp(name, ".bss")    == 0) return bss_off;
    return 0xFFFFFFFF;
}



//Общая реализация загрузки, релокации и вызова драйвера
//
//free_image = true  — образ прочитан с диска через kmalloc, освобождаем его через kfree после вызова driver_main()/get_api()
//free_image = false — образ уже лежит в памяти (модуль загрузчика/ядро), не трогаем
//file_size используется только для sanity-проверки границы области драйверов. Для вызовов без известного размера
//(load_driver_elf) передаём 0 — проверка пропускается
static void *load_driver_elf_common(uint8_t *elf_image, uint32_t file_size,
                                    struct boot_info *boot, bool free_image)
{
    if (!elf_image || !boot)
    {
        if (free_image) kfree(elf_image);
        return NULL;
    }

    //---- Магия ----
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)elf_image;
    if (ehdr->ident[0] != 0x7F || ehdr->ident[1] != 'E' ||
            ehdr->ident[2] != 'L'  || ehdr->ident[3] != 'F')
    {
        if (free_image) kfree(elf_image);
        draw_string(10, 200, "BAD ELF MAGIC");
        return NULL;
    }

    //Защита области драйверов
    //Область 12..16 MB (DRIVER_LOAD_ADDR..0x1000000) — 4 MB
    //file_size >= суммарный offset (в ELF есть заголовки/reloc), поэтому проверка по file_size
    if (file_size != 0 && driver_start_load_addr + file_size > 0x1000000)
    {
        if (free_image) kfree(elf_image);
        draw_string(10, 300, "DRV OVERFLOW");
        for(;;);
    }

    //Секции и строки
    Elf32_Shdr *sections = (Elf32_Shdr *)(elf_image + ehdr->shoff);
    char *shstrtab = (char *)(elf_image + sections[ehdr->shstrndx].offset);

    Elf32_Shdr *text   = find_section(ehdr, sections, shstrtab, ".text");
    Elf32_Shdr *rodata = find_section(ehdr, sections, shstrtab, ".rodata");
    Elf32_Shdr *data   = find_section(ehdr, sections, shstrtab, ".data");
    Elf32_Shdr *bss    = find_section(ehdr, sections, shstrtab, ".bss");
    Elf32_Shdr *symtab = find_section(ehdr, sections, shstrtab, ".symtab");
    Elf32_Shdr *strtab = find_section(ehdr, sections, shstrtab, ".strtab");

    if (!text)
    {
        if (free_image) kfree(elf_image);
        draw_string(10, 220, "NO .text SECTION");
        return NULL;
    }

    //Сбор .rel.*
    Elf32_Shdr *rel_sections[20];
    int rel_count = 0;
    for (int i = 0; i < ehdr->shnum && rel_count < 20; i++)
    {
        char *name = shstrtab + sections[i].name;
        if (strncmp(name, ".rel.", 5) == 0)
            rel_sections[rel_count++] = &sections[i];
    }

    //Раскладка секций в загруженном модуле
    int32_t offset = 0;
    uint32_t text_off = align_up(offset, text->addralign);
    offset = text_off + text->size;

    uint32_t rodata_off = 0;
    if (rodata)
    {
        rodata_off = align_up(offset, rodata->addralign);
        offset = rodata_off + rodata->size;
    }

    uint32_t data_off = 0;
    if (data)
    {
        data_off = align_up(offset, data->addralign);
        offset = data_off + data->size;
    }

    uint32_t bss_off = 0;
    if (bss)
    {
        bss_off = align_up(offset, bss->addralign);
        offset = bss_off + bss->size;
    }

    //Копирование секций
    uint8_t *dst = (uint8_t *)driver_start_load_addr;
    memcpy(dst + text_off, elf_image + text->offset, text->size);
    if (rodata) memcpy(dst + rodata_off, elf_image + rodata->offset, rodata->size);
    if (data)   memcpy(dst + data_off,   elf_image + data->offset,   data->size);
    if (bss)    memset(dst + bss_off, 0, bss->size);

    //Таблицы символов
    Elf32_Sym *syms = NULL;
    char *strings = NULL;
    uint32_t sym_count = 0;
    if (symtab && strtab)
    {
        syms = (Elf32_Sym *)(elf_image + symtab->offset);
        strings = (char *)(elf_image + strtab->offset);
        sym_count = symtab->size / sizeof(Elf32_Sym);
    }

    //Применение релокаций
    for (int r_sec = 0; r_sec < rel_count; r_sec++)
    {
        Elf32_Shdr *rel_sh = rel_sections[r_sec];

        uint32_t target_off = section_base_for_shndx(sections, rel_sh->info, shstrtab,
                              text_off, rodata_off, data_off, bss_off);
        if (target_off == 0xFFFFFFFF) continue;

        uint32_t target_sh_addr = sections[rel_sh->info].addr;

        Elf32_Rel *rels = (Elf32_Rel *)(elf_image + rel_sh->offset);
        int num = rel_sh->size / sizeof(Elf32_Rel);

        for (int i = 0; i < num; i++)
        {
            uint32_t sym_idx = ELF32_R_SYM(rels[i].info);
            uint32_t type    = ELF32_R_TYPE(rels[i].info);
            if (sym_idx >= sym_count) continue;

            Elf32_Sym *sym = &syms[sym_idx];

            //Диагностика на случай, если забыли флаг -fno-common при компиляции
            if (sym->st_shndx == 0xFFF2)  //SHN_COMMON
            {
                com_puts("[LD] WARN: COMMON symbol (нужен -fno-common): ");
                com_puts(strings + sym->st_name);
                com_puts("\n");
                continue;
            }

            uint32_t sym_sec_off = section_base_for_shndx(sections, sym->st_shndx, shstrtab,
                                   text_off, rodata_off, data_off, bss_off);
            if (sym_sec_off == 0xFFFFFFFF) continue;

            //offset внутри секции = st_value - sh_addr (0 для -r; vaddr для --emit-relocs)
            uint32_t sym_sh_addr = sections[sym->st_shndx].addr;
            uint32_t sym_offset_in_section;
            if (sym_sh_addr != 0 && sym->st_value >= sym_sh_addr)
                sym_offset_in_section = sym->st_value - sym_sh_addr;
            else
                sym_offset_in_section = sym->st_value;

            uint32_t sym_new_addr = driver_start_load_addr + sym_sec_off + sym_offset_in_section;

            //offset релокации внутри целевой секции = r_offset - sh_addr
            uint32_t r_offset_in_section;
            if (target_sh_addr != 0 && rels[i].offset >= target_sh_addr)
                r_offset_in_section = rels[i].offset - target_sh_addr;
            else
                r_offset_in_section = rels[i].offset;

            uint32_t patch_addr = driver_start_load_addr + target_off + r_offset_in_section;
            uint32_t *patch = (uint32_t *)patch_addr;

            if (type == R_386_32)
            {
                //addend нужно восстановить, потому что линкер уже впёк его в старое значение по адресу patch_addr
                uint32_t old = *patch;
                uint32_t addend;
                if (ehdr->type == 1)           //ET_REL (-r): old == addend
                {
                    addend = old;
                }
                else                           //ET_EXEC (--emit-relocs): old == sym_vaddr + addend
                {
                    addend = old - sym->st_value;
                }
                *patch = sym_new_addr + addend;
            }
            else if (type == R_386_PC32)
            {
                *patch = sym_new_addr - patch_addr - 4;
            }
        }
    }

    //Поиск driver_main / driver_get_api
    uint32_t main_offset = 0;
    uint32_t api_offset  = 0;
    for (uint32_t i = 0; i < sym_count; i++)
    {
        const char *name = strings + syms[i].st_name;
        bool is_main = (strcmp(name, "driver_main") == 0);
        bool is_api  = (strcmp(name, "driver_get_api") == 0);
        if (!is_main && !is_api) continue;

        uint32_t sec_off = section_base_for_shndx(sections, syms[i].st_shndx, shstrtab,
                           text_off, rodata_off, data_off, bss_off);
        if (sec_off == 0xFFFFFFFF) continue;

        uint32_t sym_offset_in_section = syms[i].st_value - sections[syms[i].st_shndx].addr;
        uint32_t final_off = sec_off + sym_offset_in_section;

        if (is_main) main_offset = final_off;
        else         api_offset  = final_off;
    }

    if (main_offset == 0 || api_offset == 0)
    {
        if (free_image) kfree(elf_image);
        draw_string(10, 240, "NO DRIVER ENTRY OR API EXPORT");
        return NULL;
    }

    //Вызов драйвера
    void (*driver_main)(struct boot_info *) = (void *)(driver_start_load_addr + main_offset);
    void *(*get_api)(void) = (void *(*)(void))(driver_start_load_addr + api_offset);

    driver_main(boot);
    void *api_return_ptr = get_api();

    if (free_image) kfree(elf_image);

    if (api_return_ptr)
    {
        //gap 0x10000 — чтобы следующий драйвер не заезжал в хвост .bss текущего
        driver_start_load_addr += (align_up(offset, 0x1000) + 0x10000) & ~0xFFFu;
    }

    return api_return_ptr;
}


//Обёртки

//Образ уже в памяти (bootloader-модуль). Не освобождаем, размер неизвестен
void *load_driver_elf(unsigned char *elf_image, struct boot_info *boot)
{
    return load_driver_elf_common(elf_image, 0, boot, false);
}

//Образ в памяти по адресу addr, размер size. Не освобождаем (память принадлежит загрузчику/ядру, а не нам)
void *load_driver_elf_mem(uint32_t addr, uint32_t size, struct boot_info *boot)
{
    return load_driver_elf_common((uint8_t *)addr, size, boot, false);
}

//Образ читается с диска и освобождается после вызова
void *load_driver_elf_from_disk(const char *path, struct boot_info *boot)
{
    if (!path || !boot) return NULL;

    fs_stat_t st;
    memset(&st, 0, sizeof(fs_stat_t));

    int stat_res = vfs_stat(path, &st);
    if (stat_res != FS_OK)
    {
        com_puts("[KERNEL ERROR] load_driver: Cannot stat driver file: ");
        com_puts(path);
        com_puts("\n");
        return NULL;
    }

    uint32_t file_size = st.size;
    if (file_size == 0 || file_size > 512 * 1024)
    {
        com_puts("[KERNEL ERROR] load_driver: Invalid ELF file size: ");
        com_puthex(file_size);
        com_puts("\n");
        return NULL;
    }

    int fd = vfs_open(path, FS_OPEN_READ);
    if (fd < 0)
    {
        com_puts("[KERNEL ERROR] load_driver: Cannot open file: ");
        com_puts(path);
        com_puts("\n");
        return NULL;
    }

    uint8_t *elf_image = (uint8_t *)kmalloc(file_size);
    if (!elf_image)
    {
        vfs_close(fd);
        com_puts("[KERNEL ERROR] load_driver: Out of memory for ELF buffer\n");
        return NULL;
    }
    memset(elf_image, 0, file_size);

    int bytes_read = 0;
    while (bytes_read < (int)file_size)
    {
        int chunk = file_size - bytes_read;
        if (chunk > 4096) chunk = 4096;
        int r = vfs_read(fd, elf_image + bytes_read, chunk);
        if (r <= 0) break;
        bytes_read += r;
    }
    vfs_close(fd);

    if (bytes_read < (int)file_size)
    {
        kfree(elf_image);
        com_puts("[KERNEL ERROR] load_driver: Read error or truncated file\n");
        return NULL;
    }

    return load_driver_elf_common(elf_image, file_size, boot, true);
}
