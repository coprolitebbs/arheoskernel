#include "include-kernel/use_drivers.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/driver_elf.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/isr.h"
#include "include-kernel/lib.h"
#include "include-kernel/vmm.h"
#include "include-kernel/task.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/kconsole.h"
#include "drivers/include_drivers/drv_format.h"
#include "include-kernel/config.h"
#include "include-kernel/kconsole.h"

//указатель на api драйвера framebuffer
fb_driver_api_t *fb = 0;

fs_driver_api_t *main_filesystem_fs_api = 0;

fs_driver_t fs_apis[FILESYSTEMS_API_MAX];

kbd_driver_api_t *kbd_driver_api = 0;

//char* additional_drivers_list = "kbdps2.drv";


void drivers_init(void){
    module_info_t *m = 0;

    m = &boot_info->modules[0];
    fb = (fb_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);

    if(!fb){
        char *err = "FB Driver load error";
        draw_string(10, 20, err);
    }

    fb->malloc_ptr = kmalloc;
    fb->free_ptr   = kfree;

    m = &boot_info->modules[1];
    fs_apis[0].api = (fs_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);
    fs_apis[0].type = FS_FAT12;
    fs_apis[0].api->floppy_irq_fired_ptr = &floppy_irq_fired;
    fs_apis[0].api->schedule_lock_ptr = &kernel_schedule_lock;
    fs_apis[0].api->malloc_ptr = kmalloc;
    fs_apis[0].api->free_ptr   = kfree;

    if(!fs_apis[0].api){
        char *err = "FAT12 drv load error";
        draw_string(10, 30, err);
    }
    m = &boot_info->modules[2];
    fs_apis[1].api = (fs_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);
    fs_apis[1].type = FS_EXT2;
    fs_apis[1].api->floppy_irq_fired_ptr = &floppy_irq_fired;
    fs_apis[1].api->schedule_lock_ptr = &kernel_schedule_lock;
    fs_apis[1].api->malloc_ptr = kmalloc;
    fs_apis[1].api->free_ptr   = kfree;

    if(!fs_apis[1].api){
        char *err = "EXT2 drv load error";
        draw_string(10, 230, err);
    }

}


int load_additional_drivers(cfg_file_t *cfg){
    int res = DRV_STATUS_OK;
    kprintf("Loading additional drivers...\n");
    //Открываем конфигурационный файл через VFS
    //cfg_file_t *cfg = cfg_open("/config/boot/boot.cfg", FS_OPEN_READ,true);
    if (cfg < 0) {
        kprintf("Boot configuration file not found\n");
        return DRV_STATUS_LOAD_ERROR;
    }
    //Вычитываем количество драйверов функцией из config.c
    int drivers_count = cfg_read_int(cfg, "main", "startup_drivers_count", 0);
    if (drivers_count <= 0){
        //cfg_close(cfg);
        return DRV_STATUS_OK;
    }
    //Выделяем временные буферы под маршалинг строк в куче Ring 0
    char *section_name = (char *)kmalloc(64);
    char *drv_name = (char *)kmalloc(64);
    char *drv_type = (char *)kmalloc(32);
    char *full_drv_path = (char *)kmalloc(128);

    if (!section_name || !drv_name || !drv_type || !full_drv_path) {
        if (section_name) kfree(section_name);
        if (drv_name) kfree(drv_name);
        if (drv_type) kfree(drv_type);
        if (full_drv_path) kfree(full_drv_path);
        //cfg_close(cfg);
        return DRV_STATUS_LOAD_ERROR;
    }
    //Последовательный перебор секций [driver_1], [driver_2]...
    for (int i = 1; i <= drivers_count; i++){
        // Собираем имя целевой секции
        memset(section_name, 0, 64);
        strcpy(section_name, "driver_");

        char num_buf[16];
        itoa(i, num_buf, 10);
        strcat(section_name, num_buf);

        memset(drv_name, 0, 64);
        memset(drv_type, 0, 32);
        memset(full_drv_path, 0, 128);

        cfg_read_string(cfg, section_name, "name", "", drv_name, 64);
        cfg_read_string(cfg, section_name, "type", "", drv_type, 32);

        //Если параметры в секции отсутствуют или повреждены — пропускаем слот
        if (drv_name[0] == '\0' || drv_type[0] == '\0') {
            kprintf("Slot [%s] parsing failed or empty\n", section_name);
            continue;
        }

        //kprintf("Processing slot [%s]: type=%s, file=%s\n", section_name, drv_type, drv_name);

        //Генерируем абсолютный путь к ELF-драйверу на диске VFS
        strcpy(full_drv_path, "/adrv/");
        strcat(full_drv_path, drv_name);

        //Диспетчеризация по системному типу устройства
        if (strcmp(drv_type, "kbd_main") == 0) {
            kbd_driver_api = (kbd_driver_api_t *)load_driver_elf_from_disk(full_drv_path, boot_info);

            if (!kbd_driver_api) {
                res = DRV_STATUS_LOAD_ERROR;
                draw_string(10, 30, "KBD Driver load error");
            } else {
                //Пришиваем ядерный рантайм к интерфейсу подгруженного ELF-модуля
                kbd_driver_api->malloc_ptr = kmalloc;
                kbd_driver_api->free_ptr   = kfree;
                kbd_driver_api->yield_ptr  = sys_yield;
                kprintf("Driver %s loaded and registered successfully\n", drv_name);
            }
        } else {
            kprintf("Driver %s skipped (type %s not supported yet)\n", drv_name, drv_type);
        }
    }

    kfree(section_name);
    kfree(drv_name);
    kfree(drv_type);
    kfree(full_drv_path);

    //cfg_close(cfg);
    return res;
}
