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

#include "include-kernel/use_drivers.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/driver_elf.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/isr.h"
#include "include-kernel/lib.h"
#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"
#include "include-kernel/task.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/kconsole.h"
#include "drivers/include_drivers/drv_format.h"
#include "include-kernel/config.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/font_data.h"
#include "include-kernel/locale.h"
#include "include-kernel/font.h"
#include "include-kernel/kload_screen.h"
#include "include-kernel/usb_registry.h"

//указатель на api драйвера framebuffer
fb_driver_api_t *fb = 0;

fs_driver_api_t *main_filesystem_fs_api = 0;

fs_driver_t fs_apis[FILESYSTEMS_API_MAX];

kbd_driver_api_t *kbd_driver_api = 0;

block_driver_api_t *blockdevice_driver = 0;

usb_stack_driver_api_t *usb_driver_api = 0;

//char* additional_drivers_list = "kbdps2.drv";
//обертка для получения локали
static const void *kernel_get_active_locale(void){
    return (const void *)locale_get_active();
}


/* Универсальный USB DMA-аллокатор.
   Возвращает физически непрерывный блок, identity-mapped в VA-пространство
   ядра (а значит — и в адресное пространство драйвера, который живёт в
   том же CR3). Используется драйвером для QH/TD и больших transfer-буферов. */
static void *kernel_usb_dma_alloc(uint32_t size, uint32_t *phys_out){
    if (size == 0 || !phys_out) return NULL;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t phys  = (uint32_t)pmm_alloc_pages(pages);
    if (!phys) return NULL;
    *phys_out = phys;
    return (void*)phys;
}

static void kernel_usb_dma_free(void *virt, uint32_t size){
    if (!virt || size == 0) return;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free_pages(virt, pages);
}


/* Тики ядра для драйверов: PIT-счётчик, 100 Гц. Просто читаем глобал. */
static uint32_t kernel_get_ticks(void){
    return tick_count;
}



void drivers_init(void){
    module_info_t *m = 0;

    m = &boot_info->modules[0];

    description_driver_api_t *fb_descr = (description_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);
    if(fb_descr && fb_descr->type == DESCR_FRAMEBUFFER_DRIVER)
        fb = (fb_driver_api_t *)fb_descr->struct_ptr;
    //(fb_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);

    if(!fb){
        char *err = "FB Driver load error";
        draw_string(10, 20, err);
        for(;;);
    }

    fb->malloc_ptr = kmalloc;
    fb->free_ptr = kfree;
    fb->font = (const uint8_t (*)[8])font_data;
    //fb->active_locale = locale_get_active();
    fb->get_active_locale_ptr = kernel_get_active_locale;
    fb->font_lookup_ptr = (const void *(*)(const void *, uint32_t))font_lookup;


    m = &boot_info->modules[1];
    fb_descr = (description_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);
    if(fb_descr && fb_descr->type == DESCR_BLOCKDEVICE_DRIVER)
        blockdevice_driver = (block_driver_api_t *)fb_descr->struct_ptr;
    if(!blockdevice_driver){
        char *err = "Blockdevice Driver load error";
        draw_string(10, 30, err);
        for(;;);
    } else {
        blockdevice_driver->malloc_ptr = kmalloc;
        blockdevice_driver->free_ptr   = kfree;
        blockdevice_driver->yield_ptr  = sys_yield;
        if (!blockdevice_driver->enabled){
            //kprintf("Block driver: no ATA devices found, keeping legacy ATA path\n");
            //blockdevice_driver->enabled = false;
            //main_filesystem_fs_api->block_device_driver_enabled = false;
        }
        block_info_t bi;
        int gi = blockdevice_driver->get_info(0, &bi);
        if (gi != FS_OK){
            blockdevice_driver->enabled = false;
            //main_filesystem_fs_api->block_device_driver_enabled = false;
        }
        uint32_t real_id = 0;
        if (blockdevice_driver->resolve_boot_drive(boot_info->boot_drive, &real_id) == FS_OK && real_id == 0){
            //Ничего не меняем: vfs mount 0 уже указывает на device 0
        } else if (real_id != 0){
            //kprintf("WARN: boot drive maps to block id %d, root ext2 was mounted from ATA master - keeping legacy path to avoid remount\n", real_id);
            // Не вызываем relink; ext2 продолжит читать через legacy ATA
            // (или реализовать полноценный remount через vfs_unmount/vfs_mount)
            //main_filesystem_fs_api->relink_drive_id(0,real_id);
            //+ любые другие смонтированные тома по индексам
        }

        //main_filesystem_fs_api->block = blockdevice_driver;
    }
    //load_screen_add_string("Try to load usb");
    //USB driver
    m = &boot_info->modules[2];
    fb_descr = (description_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);
    if(fb_descr && fb_descr->type == DESCR_USB_DRIVER)
        usb_driver_api = (usb_stack_driver_api_t *)fb_descr->struct_ptr;
    if(usb_driver_api){
        usb_driver_api->schedule_lock_ptr = &kernel_schedule_lock;
        usb_driver_api->block = blockdevice_driver;
        usb_driver_api->vmm_map_pages_ptr = vmm_map_pages;
        usb_driver_api->vmm_map_pages_uncached_ptr = vmm_map_pages_uncached;
        usb_driver_api->malloc_ptr = kmalloc;
        usb_driver_api->free_ptr   = kfree;
        usb_driver_api->yield_ptr = sys_yield;
        usb_driver_api->debug7 = 0;
        usb_driver_api->debug8 = 0;
        usb_driver_api->debug9 = 0;
        usb_driver_api->load_screen_add_string = load_screen_add_string;
        usb_driver_api->dma_alloc_ptr = kernel_usb_dma_alloc;
        usb_driver_api->dma_free_ptr = kernel_usb_dma_free;
        usb_driver_api->get_ticks = kernel_get_ticks;

        //load_screen_add_string("clearing registry");
        usb_registry_clear();

        //load_screen_add_string("Starting usb");
        int res = usb_driver_api->usb_start();
        (void)res;

         /* Драйвер сам заполняет devices[] и device_count.
           Ядро просто перекладывает записи в реестр. */
        for (uint32_t i = 0; i < usb_driver_api->device_count; i++){
            usb_registry_add(&usb_driver_api->devices[i]);
        }


        load_screen_add_string("USB dev: VID=%x PID=%x class=%x",usb_driver_api->vid, usb_driver_api->pid,usb_driver_api->dev_class);
        load_screen_add_string("USB: %d device(s) enumerated",usb_driver_api->device_count);
        load_screen_add_string("USB p0 step=%d res=%x vp=%x dc=%x",
            (int)usb_driver_api->debug1, usb_driver_api->debug2,
            usb_driver_api->debug5, usb_driver_api->debug6);
        load_screen_add_string("USB p1 step=%d res=%x vp=%x dc=%x",
            (int)usb_driver_api->debug3, usb_driver_api->debug4,
            usb_driver_api->debug7, usb_driver_api->debug8);
        //load_screen_add_string("USB last error: %x",usb_driver_api->last_error);
        //if (usb_driver_api->block_id != 0xFFFFFFFFu && usb_driver_api->device_count > 0)
            //load_screen_add_string("USB registered as block id=%d", usb_driver_api->block_id);
        //if (usb_driver_api->device_count > 1)
            //load_screen_add_string("USB: %d devices total", usb_driver_api->device_count);
        //if (usb_driver_api->last_error)
            //load_screen_add_string("USB last error: %x", usb_driver_api->last_error);
        //load_screen_add_string("EHCI P%d PORTSC:%x PSPD:%d",usb_driver_api->debug7,usb_driver_api->debug8,usb_driver_api->debug9);
        //for(;;);
    } else {
        load_screen_add_string("USB driver load error");
        //for(;;);
    }

    //load_screen_add_string("Try to load fat12");
    m = &boot_info->modules[3];
    fb_descr = (description_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);
    if(fb_descr && fb_descr->type == DESCR_FILESYSTEM_DRIVER){
        //fs_apis[0].api = (fs_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);
        fs_apis[0].api = (fs_driver_api_t *)fb_descr->struct_ptr;
        fs_apis[0].type = FS_FAT12;
        fs_apis[0].api->floppy_irq_fired_ptr = &floppy_irq_fired;
        fs_apis[0].api->schedule_lock_ptr = &kernel_schedule_lock;
        fs_apis[0].api->malloc_ptr = kmalloc;
        fs_apis[0].api->free_ptr   = kfree;
        if(blockdevice_driver && blockdevice_driver->enabled ){
            fs_apis[0].api->block = blockdevice_driver;
            fs_apis[0].api->block_device_driver_enabled = true;
        } else {
            fs_apis[0].api->block_device_driver_enabled = false;
        }

        //fs_apis[0].api->printf_ptr = kprintf;
        if(!fs_apis[0].api){
            //char *err = "FAT12 drv load error";
            //draw_string(10, 30, err);
            load_screen_add_string("EXT2 drv load error");
        }
    }

    m = &boot_info->modules[4];
    fb_descr = (description_driver_api_t *)load_driver_elf_mem(m->start,m->size,boot_info);

    if(fb_descr && fb_descr->type == DESCR_FILESYSTEM_DRIVER){
        fs_apis[1].api = (fs_driver_api_t *)fb_descr->struct_ptr;
        fs_apis[1].type = FS_EXT2;
        fs_apis[1].api->floppy_irq_fired_ptr = &floppy_irq_fired;
        fs_apis[1].api->schedule_lock_ptr = &kernel_schedule_lock;
        fs_apis[1].api->malloc_ptr = kmalloc;
        fs_apis[1].api->free_ptr   = kfree;
        if (blockdevice_driver && blockdevice_driver->enabled){
            fs_apis[1].api->block = blockdevice_driver;
            fs_apis[1].api->block_device_driver_enabled = true;
        } else {
            fs_apis[1].api->block_device_driver_enabled = false;
        }
        if(!fs_apis[1].api){
            //char *err = "EXT2 drv load error";
            //draw_string(10, 230, err);
            load_screen_add_string("EXT2 drv load error");
        }
    }
    //load_screen_add_string("FB & Block & USB & Fat12 & EXT2 drivers loaded");
}


int load_additional_drivers(cfg_file_t *cfg){
    int res = DRV_STATUS_OK;
    //Открываем конфигурационный файл через VFS
    //cfg_file_t *cfg = cfg_open("/config/boot/boot.cfg", FS_OPEN_READ,true);
    if (cfg < 0) {
        //kprintf("Boot configuration file not found\n");
        load_screen_add_string("Boot configuration file not found");
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
    char *full_drv_path = (char *)kmalloc(128);

    if (!section_name || !drv_name || !full_drv_path) {
        if (section_name) kfree(section_name);
        if (drv_name) kfree(drv_name);
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
        memset(full_drv_path, 0, 128);

        cfg_read_string(cfg, section_name, "name", "", drv_name, 64);

        //Если параметры в секции отсутствуют или повреждены — пропускаем слот
        if (drv_name[0] == '\0' /* || drv_type[0] == '\0'*/) {
            //kprintf("Slot [%s] parsing failed or empty\n", section_name);
            load_screen_add_string("Slot [%s] parsing failed or empty", section_name);
            continue;
        }
        //kprintf("Processing slot [%s]: type=%s, file=%s\n", section_name, drv_type, drv_name);
        //Генерируем абсолютный путь к ELF-драйверу на диске VFS
        strcpy(full_drv_path, "/adrv/");
        strcat(full_drv_path, drv_name);

        //Диспетчеризация по системному типу устройства
        description_driver_api_t *fb_descr = (description_driver_api_t *)load_driver_elf_from_disk(full_drv_path, boot_info);
        if(fb_descr && fb_descr->type > 0 && fb_descr->struct_ptr){
            switch(fb_descr->type){
                case DESCR_KEYBOARD_DRIVER:{  //Драйвер клавиатуры
                    kbd_driver_api = (kbd_driver_api_t *)fb_descr->struct_ptr;
                    if (!kbd_driver_api){
                        res = DRV_STATUS_LOAD_ERROR;
                        //draw_string(10, 30, "KBD Driver load error");
                        load_screen_add_string("KBD Driver load error");
                        //for(;;);
                    } else {
                        //Пришиваем ядерный рантайм к интерфейсу подгруженного ELF-модуля
                        kbd_driver_api->malloc_ptr = kmalloc;
                        kbd_driver_api->free_ptr   = kfree;
                        kbd_driver_api->yield_ptr  = sys_yield;
                        load_screen_add_string("Driver %s loaded ", drv_name);
                        //kprintf("Driver %s loaded and registered successfully\n", drv_name);
                    }
                    break;
                }
                /*case DESCR_USB_DRIVER:{
                    usb_driver_api = (usb_stack_driver_api_t *)fb_descr->struct_ptr;
                    if (!usb_driver_api){
                        res = DRV_STATUS_LOAD_ERROR;
                        load_screen_add_string("USB Driver load error");
                    } else {
                        usb_driver_api->schedule_lock_ptr = &kernel_schedule_lock;

                        load_screen_add_string("Driver %s loaded ", drv_name);
                        load_screen_add_string("USB: VID=%x PID=%x",usb_driver_api->debug1, usb_driver_api->debug2);
                        for(;;);

                    }


                    break;
                }*/

                default:
                    kprintf("Driver %s skipped (type %d not supported yet)\n", drv_name, fb_descr->type);
                    break;
            }
        } else {
            kprintf("Driver %s not found\n",full_drv_path);
        }

    }

    kfree(section_name);
    kfree(drv_name);
    kfree(full_drv_path);
    return res;
}

