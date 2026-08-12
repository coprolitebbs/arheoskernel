#include "include-kernel/kernel.h"
#include "include-kernel/gdt.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/font_data.h"
#include "include-kernel/task.h"
#include "include-kernel/pmm.h"
#include "include-kernel/vmm.h"
#include "include-kernel/debug.h"

//#include "include-kernel/driver_elf.h"
//#include "include-kernel/fb_driver_bin.h"
#include "drivers/include_drivers/drv_format.h"
#include "drivers/fat12_driver/include/fat12_internal.h"
//#include "include-kernel/use_drivers.h"



void _start(void) {
    // Получаем параметры VBE
	boot_info_t *info;


    asm volatile("movl %%ebx,%0" : "=r"(info));

    bootinfo_init(info);



	if(boot_info){
        lfb_addr = boot_info->framebuffer;
        pitch    = boot_info->pitch;
        bytes_pp = boot_info->bpp / 8;
    }


	asm volatile("cli");

	fill_screen(0, 0, 0xFF);

	//draw_string(0, 232, boot_info->modules[2].name);
	//draw_hex_dword(boot_info->boot_drive, 20, 50);

	//draw_hex_dump_spaced(0x5200,64,0,150);
    //draw_hex_dump_spaced((uint32_t)boot_info->modules,64,0,450);


	gdt_init();

    tss_init();

	update_tss_esp0(0x90000); // адрес стека ядра

	// ---- Инициализация IDT и PIC ----
    idt_init();      // заполнение и загрузка IDT
    pic_init();      // перенастройка PIC
	pit_init();

	// ---- Берем карту памяти из загрузчика ----
    uint16_t num_entries = *(uint16_t*)0x6000;
    struct e820_entry *entries = (struct e820_entry*)0x6002;

	// ---- Инициализация физического аллокатора памяти ----
	pmm_init(num_entries, entries);
	// ---- Инициализация Virtual Memory Manager ----
    vmm_init(lfb_addr);

	update_tss_esp0(0x90000);

	init_tasking();

	// ---- Drivers test ----
    drivers_init();


    if(boot_info->fs->type == FS_FAT12){
        //draw_hex_dword(boot_info->fs->type, 20, 50);
        int r = f12dr->mount(0);
        draw_hex_dword(r, 20, 150);
        fat12_fs_t *vol = (fat12_fs_t *)f12dr->get_volume(0);

        fat12_dirent_t eentry;
        const char nnn[] = "KERNEL     ";
        int l = f12dr->lookup(
            vol,
            nnn,
            &eentry
        );

        int fd = f12dr->open(vol,"KERNEL     ",0);
        draw_hex_dword(fd,10,170);
        uint8_t buffer[8];
        int np = f12dr->read(fd,buffer,8);
        draw_hex_dword(np,10,190);

        draw_hex_dword(f12dr->debug_a(), 10, 210);
        draw_hex_dword(f12dr->debug_b(), 10, 220);
        draw_hex_dword(f12dr->debug_c(), 10, 230);
        draw_hex_dword(f12dr->debug_array(), 10, 240);
        draw_hex_dword(f12dr->debug_size(), 10, 250);

        draw_hex_dump_spaced(buffer,8,10,350);

    }

	// --------

	task_t *task1;
	task_t *task2;

	task1=create_task(0x40000000);
	task2=create_task(0x40000000);


	load_user_program(task1,0x40000000,user_prog,sizeof(user_prog));
	load_user_program(task2,0x40000000,user_prog2,sizeof(user_prog2));

	update_tss_esp0(0x90000);

    task_t *first=get_first_task();

	if(first){

		current_task=first;

		current_task->state=TASK_RUNNING;

		vmm_switch_directory(current_task->page_dir_phys);

		update_tss_esp0(current_task->kernel_stack);

		switch_task(current_task->esp);
	}

	for(;;);

}




void switch_to_usermode(uint32_t entry) {
    asm volatile(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "push $0x23\n"
        "push $0x90000\n"
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"
        "push %%eax\n"
        "push $0x1B\n"
        "push %0\n"

        "iret"
        : : "r"(entry) : "memory", "eax", "edx"
    );
}



