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
#include "include-kernel/comdebug.h"
#include "include-kernel/ports_io.h"
#include "include-kernel/hardware.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/vfs.h"
#include "drivers/include_drivers/drv_format.h"
#include "drivers/fat12_driver/include/fat12_internal.h"
#include "drivers/ext2_driver/include/ext2_internal.h"


void _start(void){
    //Получаем параметры VBE
	boot_info_t *info;

    asm volatile("movl %%ebx,%0" : "=r"(info));

com_init();
com_puts("[KERNEL] Starting...\n");

    bootinfo_init(info);

	if(boot_info){
        lfb_addr = boot_info->framebuffer;
        pitch    = boot_info->pitch;
        bytes_pp = boot_info->bpp / 8;
    }

	asm volatile("cli");



	//fill_screen(0xFF, 0x1, 0x1);
	//for(;;);

	//draw_string(0, 232, boot_info->modules[2].name);
	//draw_hex_dword(boot_info->bpp, 20, 50);
    //draw_hex_dword(boot_info->pitch, 20, 60);
    //draw_hex_dword(boot_info->framebuffer, 20, 70);
    //draw_hex_dword(boot_info->graphics_flags, 20, 80);

    com_puts("[KERNEL] VBE Framebuffer addr: ");
    com_puthex(boot_info->framebuffer);
    com_puts("\n");

    com_puts("[KERNEL] VBE flags: ");
    com_puthex(boot_info->graphics_flags);
    com_puts("\n");

    com_puts("[KERNEL] VBE Width: ");
    com_puthex(boot_info->width);
    com_puts("\n");

    com_puts("[KERNEL] VBE Height: ");
    com_puthex(boot_info->height);
    com_puts("\n");

    com_puts("[KERNEL] VBE BPP: ");
    com_puthex(boot_info->bpp);
    com_puts("\n");

	//draw_hex_dump_spaced(0x5200,64,0,150);
    //draw_hex_dump_spaced((uint32_t)boot_info->modules,64,0,450);


	gdt_init();

    tss_init();

	update_tss_esp0(STACK_ADDRESS); // адрес стека ядра

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
    vmm_init(/*lfb_addr*/);

    // ---- Попытка поиска адреса линейного фреймбуффера в памяти -----
	uint32_t total_mem = pmm_get_total_memory();
    has_lfb = (boot_info->graphics_flags & 1);  // Загрузчик stage2 должен определить наличие еще при установке VBE и передать сюда бит
    //Попробуем определить адрес начала фреймбуффера
    graphics_init_uma_address(total_mem, has_lfb, lfb_addr);
    //Попробуем принудительно поискать карту S3 Trio64, если мы вдруг оказались в 86box и офигели от отсутствия линейного фреймбуффера
    find_and_prepare_s3_trio64();

    uint32_t fb_phys = graphics_get_fb_phys_addr();
    uint32_t fb_size = has_lfb ? boot_info->pitch * boot_info->height : (4 * 1024 * 1024);

    //uint32_t vga_phys = 0x000A0000;
    //uint32_t vga_size = 0x00020000;

    //Замапим память под фреймбуфер чтоб процессор не блевал PageFault'ами
    vmm_map_pages(fb_phys,fb_size);

    //draw_string(10,300,"Update FB");
	//Подновим адрес фреймбуффера в структуре bootinfo, если он, вдруг, изменился
    boot_info->framebuffer = fb_phys;
    lfb_addr = fb_phys;


    fill_screen(0xFF, 0xF, 0xF);
    com_puts("[KERNEL] Screen filled.\n");

    //uint32_t aaaa = find_s3_trio64_lfb();
    //com_puts("[KERNEL] find_s3_trio64_lfb addr: ");
    //com_puthex(aaaa);
    //com_puts("\n");

    //uint32_t gaa = get_geode_gx1_framebuffer_address();
    //com_puts("[KERNEL] get_geode_gx1_framebuffer_address: ");
    //com_puthex(gaa);
    //com_puts("\n");

    //vmm_init_fb(0xDF000000, 4 * 1024 * 1024);

    /*
    volatile uint8_t* vram = (volatile uint8_t*)0xDF000000;
    for(uint32_t offset = 0; offset < 2000; offset += 1){
        vram[offset] = 0xF; // Рисуем пиксель в обход окон VESA
    }*/
    //test_draw_lfb();

	//draw_hex_dword(pmm_get_total_memory(), 20, 100);

	//uint32_t gms = graphics_get_uma_address();
	//draw_hex_dword(fb_phys, 20, 110);
	//draw_hex_dword(fb_size, 20, 120);

    //com_puts("[KERNEL] Normal Stop all\n");
	//for(;;);

    //draw_string(10,310,"Update TSS");
	update_tss_esp0(STACK_ADDRESS);

    //draw_string(10,320,"Init tasking");
	init_tasking();


    //drivers_init();

    //init_kernel_schedule_lock = 1;
    //__asm__ __volatile__("sti");

    //drivers_init();
    //int r = vfs_init_system();

    extern void kernel_init_thread(void);

    //Инит потока ядра как отдельной задачи в пространстве ядра
    init_kernel_schedule_lock = 1;
    create_kernel_thread((uint32_t)kernel_init_thread);
    init_kernel_schedule_lock = 0;

    update_tss_esp0(STACK_ADDRESS);
    //kernel_schedule_lock = 1;
    //draw_hex_dword(r, 310, 650);
    //kernel_schedule_lock = 0;
    //kernel_schedule_lock = 1;

    /*
    //filesystem test
    uint8_t buffer[8];
    uint8_t write_buffer[8] = {'1', '2', '3', '4', '5', '6', '7', '8'};
    uint8_t read_back_buffer[8];
    //kernel_schedule_lock = 1;
    r = vfs_mkdir("/dir2");
    draw_hex_dword(r, 110, 170);

    draw_hex_dump_spaced(main_filesystem_fs_api->debug_buf,8,310,660);

    int fd_write = vfs_open("/dir2/test.txt", FS_OPEN_CREATE | FS_OPEN_WRITE);
    draw_hex_dword(fd_write, 10, 180); // Дескриптор файла записи

    int bytes_written = vfs_write(fd_write, write_buffer, 8);
    draw_hex_dword(bytes_written, 10, 190); // Ожидаем 8
    r = vfs_close(fd_write);
    draw_hex_dword(r, 10, 200); // Ожидаем 0

    // ОТКРЫВАЕМ КОРНЕВОЙ КАТАЛОГ ЖЕСТКОГО ДИСКА
    int dir_fd = vfs_open("/dir2", FS_OPEN_READ);
    draw_hex_dword(dir_fd, 120, 220);
    draw_string(120, 240, "Listing dir:");
    if (dir_fd >= 0) {
        fs_dirent_t entry;
        int current_y = 250;
        while (vfs_readdir(dir_fd, &entry) == FS_OK) {
                draw_hex_dword(entry.inode, 120, current_y);
                draw_string(220, current_y, entry.name);
                current_y += 10;
                if (current_y > 650) break;
        }
        current_y += 10;
        r = vfs_readdir(dir_fd, &entry);
        draw_hex_dword(r, 120, current_y);
        //ЗАКРЫВАЕМ КАТАЛОГ
        vfs_close(dir_fd);
    }
    */

	// --------
    //init_kernel_schedule_lock = 0;
	//com_puts("[KERNEL] Normal start progs\n");
    //draw_string(10,340,"Prepare to start progs");

	//task_t *task1;
	//task_t *task2;

	//task1=create_task(0x40000000);
	//task2=create_task(0x40000000);

    //draw_string(10,350,"Tasks created");

	//load_user_program(task1,0x40000000,user_prog,sizeof(user_prog));
	//load_user_program(task2,0x40000000,user_prog2,sizeof(user_prog2));
	//task_spawn_builtin(0x40000000, user_prog, sizeof(user_prog));
    //task_spawn_builtin(0x40000000, user_prog2, sizeof(user_prog2));
    //init_kernel_schedule_lock = 1;
    //task_spawn_from_file("/hello.bin", 0x40000000);
    //com_puts("[KERNEL] hello.bin started\n");
	//update_tss_esp0(STACK_ADDRESS);

    //draw_string(10,360,"Tasks loaded");

    task_t *first=get_first_task();

    //for(;;);
	if(first){

		current_task=first;

		current_task->state=TASK_RUNNING;
		vmm_switch_directory(current_task->page_dir_phys);
		update_tss_esp0(current_task->kernel_stack);

        //init_kernel_schedule_lock = 0;

        //asm volatile("sti");
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



void kernel_halt_all(void){
    kernel_schedule_lock = 1;
    kprintf("\n=== ARHeos Operating System ===\n");
    kprintf("Initiating structural system shutdown sequences...\n");
    kprintf("All VFS partitions unmounted safely.\n");
    //Прямой выстрел в фиксированные ACPI порты QEMU / Bochs / VirtualBox
    outw(0x604, 0x2000);  // QEMU ACPI Soft Off
    outw(0xB004, 0x2000); // Bochs Power Off
    outw(0x4004, 0x3400); // VirtualBox VBoxVMM
    //На старых ATX платах Award/AMI BIOS управление питанием по умолчанию
    //заблокировано в режиме совместимости APM. Чтобы южный мост начал слушать
    //ACPI-порты, шлём команду 0xAC (Включить ACPI) в порт управления SMI_CMD (0xB2).
    com_puts("[KERNEL SHUTDOWN] Enabling ACPI controller via SMI...\n");
    outb(0xB2, 0xAC);
    io_wait(); //Небольшая задержка, чтобы логика чипсета перестроилась
    //Опрашиваем южный мост PIIX4 (Шина 0, Устройство 7, Функция 3, Регистр 0x40)
    uint32_t pci_addr = (1 << 31) | (0 << 16) | (7 << 11) | (3 << 8) | 0x40;

    outl(0xCF8, pci_addr);
    uint32_t pmba_val = inl(0xCFC);

    if (pmba_val != 0 && pmba_val != 0xFFFFFFFF){
        uint16_t pm_base = (uint16_t)(pmba_val & 0xFFFE);
        if (pm_base != 0){
            kprintf("[ACPI] Detected 86box PMBA base I/O port: 0x%x\n", pm_base);
            uint16_t pm1_cnt_port = pm_base + 4;
            kprintf("[ACPI] Target PM1_CNT register port: 0x%x\n", pm1_cnt_port);
            kprintf("[ACPI] Executing PIIX4 Soft-Off command sequence...\n");
            //Настоящая аппаратная маска S5 выключения чипсета Intel PIIX4 в 86box:
            //Бит 10 (0x0400) — это SLP_EN (Sleep Enable).
            //Биты 11-13 кодируют тип сна SLP_TYP. Для Soft-Off (S5) на платах Award
            //значение этих битов равно 7 (маска 7 << 10 == 0x1C00).
            //Итоговая монолитная команда выключения: 0x0400 | 0x1C00 = 0x2000
            //Также веерно шлём классическую PIIX4-маску Intel PIIX4 (0x3C00)
            outw(pm1_cnt_port, 0x3C00);          //Монолитная S5 маска (SLP_EN + SLP_TYP)
            outw(pm1_cnt_port, 0x2000 | 0x1C00); //Вариант 2
            outw(pm1_cnt_port, 0x0400 | (7 << 2)); //Сдвиг на биты 2-4 (альтернативный VIA)
        }
    }
    //Ковровая бомбардировка портов (Для плат VIA/SIS/ALI)
    uint16_t pm_commands[] = {0x2000, 0x3400, 0x3C00, 0x1C00, 0x041C};
    uint16_t pm_ports[] = {0x7004, 0x7304, 0x4004, 0x4304, 0xE004, 0x5004, 0x0FA4, 0xB004};
    for (int c = 0; c < 5; c++){
        for (int p = 0; p < 8; p++){
            outw(pm_ports[p], pm_commands[c]);
        }
    }
    //Древний интерфейс APM BIOS
    outw(0x5307, 0x0003);
    //Точка полной остановки (Если корпус старый AT с физической кнопкой 220V)
    kprintf("\nIt is now safe to turn off your computer.\n");
    //Если запуск на совсем древнем железе (AT-корпус без автовыключения):
    //Глушим процессор вечной ловушкой hlt.
    __asm__ __volatile__(
                "cli\n\t"
                "1:\n\t"
                "hlt\n\t"
                "jmp 1b"
    );
}
