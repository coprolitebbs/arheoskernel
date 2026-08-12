#include "include-kernel/kernel.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/isr.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/bootinfo.h"
#include "include-kernel/pmm.h"
#include "include-kernel/vmm.h"
#include "include-kernel/task.h"

void syscall_handler(struct regs *r) {

    switch (r->eax) {
        case SYS_DRAW_CHAR:
			//draw_string(10, 600, "SYSCALL");
			//draw_hex_dword(r->eip, 100, 600);
			if(r->ebx >= boot_info->width || r->ecx >=boot_info->height) return;
			//draw_char(r->ebx, r->ecx, (unsigned char)r->edx);
            kernel_draw_char(r->ebx,r->ecx,(unsigned char)r->edx,r->esi);
            break;
        case SYS_FILL_SCREEN:
            //fill_screen((unsigned char)r->ebx,(unsigned char)r->ecx,(unsigned char)r->edx);
            kernel_fill_screen(r->ebx);
            break;
		case SYS_YIELD:
			//draw_string(10,230 + 10 * (current_task->pid - 1),"SYSYIELD");
			schedule(r);
			//for(;;);
			break;
		case SYS_EXIT:
			//draw_string(10,630,"SYSEXIT");
			current_task->state = TASK_TERMINATED;
			current_task->esp = 0;
			schedule(r);
			for(;;);
			break;
		case SYS_GET_FB_INFO:
			r->eax = boot_info->framebuffer;
			r->ebx = boot_info->pitch;
			r->ecx = boot_info->bpp;
			break;
        case SYS_ALLOC_PAGE:
            uint32_t virt = current_task->heap_next;
            uint32_t phys = (uint32_t)pmm_alloc_page();
            if (!phys) {
                r->eax = 0;
                break;
            }
            uint32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
            memset((void*)phys,0,PAGE_SIZE);
            vmm_map_page( current_task->page_dir, virt, phys, flags );

            // Запоминаем принадлежность страницы текущему процессу.
            if(!vm_page_add( current_task, virt, phys, flags)){
                    /* Не смогли создать запись менеджера.
                    Поэтому страницу, которую только что
                    получили из PMM, нужно вернуть. */
                    vmm_unmap_page( current_task->page_dir, virt );
                    pmm_free_page( (void*)phys );
                    r->eax = 0;
                    break;
            }

            current_task->heap_next += PAGE_SIZE;
            r->eax = virt;
            break;


        default:
            break;
    }
}
