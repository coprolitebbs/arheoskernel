#include "include-kernel/task.h"
#include <stdint.h>
#include <stddef.h>
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/lib.h"
#include "include-kernel/gdt.h"
#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"
#include "include-kernel/kernel_heap.h"

int x_of = 8;
int shedule_count = 0;
volatile uint32_t idle_counter = 0;




/*
static void* kmalloc(uint32_t size) {
    void *ptr = (void*)mem_ptr;
    mem_ptr += size;
    return ptr;
}
*/

/*
static uint32_t kmalloc_page(void)
{
    if(mem_ptr & 0xFFF)
        mem_ptr=(mem_ptr & 0xFFFFF000) + 0x1000;

    uint32_t page=mem_ptr;

    mem_ptr += PAGE_SIZE;

    return page;
}
*/

/*
static void* kmalloc(uint32_t size)
{
    if(mem_ptr & 0xFFF)
        mem_ptr =
            (mem_ptr & 0xFFFFF000) + 0x1000;

    void *ptr=(void*)mem_ptr;

    mem_ptr += size;

    if(mem_ptr & 0xFFF)
        mem_ptr =
            (mem_ptr & 0xFFFFF000) + 0x1000;

    return ptr;
}*/


// ---- Текущая задача и очередь ----
task_t *current_task = NULL;
task_t *ready_queue = NULL;

//IDLE-задача с PID=0
static task_t idle_task;
static uint32_t idle_stack[1024];

static uint32_t next_pid = 1;
static uint32_t next_user_stack=0x800000;
//bool sw = false;






// ---- Инициализация планировщика ----
void init_tasking(void)
{
    current_task=(task_t*)kmalloc(sizeof(task_t));

    if(!current_task)
    {
        draw_string(10,120,"NO CURRENT TASK");
        for(;;);
    }


    memset(current_task,0,sizeof(task_t));


    idle_task.pid = 0;
	idle_task.state = TASK_IDLE;
	idle_task.kernel_stack = (uint32_t)idle_stack + sizeof(idle_stack);
	idle_task.page_dir = kernel_directory;
	idle_task.page_dir_phys = (uint32_t)kernel_directory;
	idle_task.user_stack = 0;
	prepare_task_stack(&idle_task,(uint32_t)idle_loop);

}





void load_user_program(task_t *task,uint32_t virt_addr,uint8_t *program,uint32_t size)
{
    uint32_t pages=(size+PAGE_SIZE-1)/PAGE_SIZE;


    for(uint32_t i=0;i<pages;i++)
    {
        uint32_t phys=(uint32_t)pmm_alloc_page();

        if(!phys)
        {
            draw_string(10,600,"NO USER MEM");
            for(;;);
        }

        memset((void*)phys,0,PAGE_SIZE);

        /*vmm_map_page(
            task->page_dir,
            virt_addr+i*PAGE_SIZE,
            phys,
			PAGE_PRESENT |
            PAGE_WRITE |
            PAGE_USER
        );
        */
        uint32_t virt = virt_addr + i * PAGE_SIZE;
        vmm_map_page(task->page_dir,virt,phys,PAGE_PRESENT | PAGE_WRITE | PAGE_USER );
        if(!vm_page_add(task, virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER)){
                draw_string(10,600,"NO VM PAGE");
                for(;;);
        }

        uint32_t copy=size-i*PAGE_SIZE;

        if(copy>PAGE_SIZE) copy=PAGE_SIZE;

        memcpy(
            (void*)phys,
            program+i*PAGE_SIZE,
            copy
        );

    }


}




// ---- Планировщик (вызывается из обработчика таймера) ----
void schedule(struct regs *r){

	//if(idle_counter < 0x4C)	draw_hex_dword(idle_counter, 500, 0 + 10 * idle_counter);

    task_t *old_task=current_task;


    // сохраняем стек текущей задачи
    if(old_task && old_task->pid!=0)
    {
        if(old_task->state==TASK_RUNNING)
        {

            old_task->esp=(uint32_t)r;


            if(old_task->pid!=0) old_task->state=TASK_READY;
        }
        else if(old_task->state==TASK_TERMINATED)
        {
            // стек больше не нужен
            old_task->esp=0;
        }
    }

	cleanup_tasks();


    // ищем следующую задачу

	task_t *next_task = NULL;

	//_test();
	if (ready_queue){
		//task_t *start = ready_queue->next;
		task_t *start = current_task->next;
		task_t *t = start;
		do{
			if (t->state == TASK_READY || t->state == TASK_IDLE){
				next_task = t;
				break;
			}
			t = t->next;
		} while (t != start);
	}


    if(!next_task){
		next_task = &idle_task;
	}


    current_task=next_task;


	current_task->state=TASK_RUNNING;

	update_tss_esp0(
		current_task->kernel_stack
	);

	vmm_switch_directory(current_task->page_dir_phys);

    /*
        Возврат в user mode

        switch_task делает iret
    */


    switch_task(current_task->esp);

	for(;;);
}





void prepare_task_stack(task_t *task, uint32_t entry)
{
    uint32_t *kstack = (uint32_t*)task->kernel_stack;

	if(task->pid==0)
	{
		/*
		После всех push стек должен выглядеть так:

			ESP -> EIP
               CS
               EFLAGS

		Поэтому записываем в обратном порядке.
		*/

		*(--kstack)=0x202;
		*(--kstack)=0x08;
		*(--kstack)=entry;

		*(--kstack)=0; // int_no
		*(--kstack)=0; // err_code

		*(--kstack)=0; // eax
		*(--kstack)=0; // ecx
		*(--kstack)=0; // edx
		*(--kstack)=0; // ebx
		*(--kstack)=0; // esp dummy
		*(--kstack)=0; // ebp
		*(--kstack)=0; // esi
		*(--kstack)=0; // edi

		*(--kstack)=0x10; // ds
		*(--kstack)=0x10; // es
		*(--kstack)=0x10; // fs
		*(--kstack)=0x10; // gs

		task->esp=(uint32_t)kstack;
		return;
	}
    /*
        iret frame

        iret берёт:

        EIP
        CS
        EFLAGS
        ESP
        SS

        Но стек растёт вниз,
        поэтому кладём наоборот.
    */

    *(--kstack) = 0x23;              // SS user data
    *(--kstack) = task->user_stack;  // ESP
    *(--kstack) = 0x202;             // EFLAGS
    *(--kstack) = 0x1B;              // CS user code
    *(--kstack) = entry;             // EIP
    /*
        как будто ISR добавил:

        push int_no
        push error
    */
    *(--kstack)=0; // err_code
    *(--kstack)=0; // int_no

    /*
        pusha

        popa снимает:

        edi
        esi
        ebp
        esp
        ebx
        edx
        ecx
        eax
    */
    *(--kstack)=0; // eax
    *(--kstack)=0; // ecx
    *(--kstack)=0; // edx
    *(--kstack)=0; // ebx
    *(--kstack)=0; // esp dummy
    *(--kstack)=0; // ebp
    *(--kstack)=0; // esi
    *(--kstack)=0; // edi
    /*
        isr_common:

        push ds
        push es
        push fs
        push gs

        поэтому pop будет:

        gs
        fs
        es
        ds
    */
    *(--kstack)=0x23; // ds
    *(--kstack)=0x23; // es
    *(--kstack)=0x23; // fs
    *(--kstack)=0x23; // gs
    task->esp=(uint32_t)kstack;
}





task_t* create_task(uint32_t entry)
{
    task_t *task = (task_t*)kmalloc(sizeof(task_t));

    if(!task) for(;;);

    memset(task,0,sizeof(task_t));


    //task->kernel_stack = kmalloc_page() + PAGE_SIZE;
    //Выделяем физическую страницу под kernel stack
    task->kernel_stack_phys = (uint32_t)pmm_alloc_page();
    if(!task->kernel_stack_phys){
        draw_string(10, 120, "NO KERNEL STACK");
        for(;;);
    }
    memset( (void*)task->kernel_stack_phys,0,PAGE_SIZE);
    //Стек растёт вниз, поэтому kernel_stack указывает на конец страницы
    task->kernel_stack = task->kernel_stack_phys + PAGE_SIZE;


    task->pid = next_pid++;
    task->state = TASK_READY;

	task->parameter = 0;

    task->heap_next = 0x40001000;

    uint32_t pd_phys;

    task->page_dir =
        vmm_create_address_space(&pd_phys);

    task->page_dir_phys = pd_phys;


    if(!task->page_dir)
        for(;;);


    task->user_stack = create_user_stack(task);


    /*
        Формируем искусственный стек,
        как будто задача была остановлена
        внутри isr_common
    */

    prepare_task_stack(task, entry);



    /*
        Добавляем в очередь
    */

    if(!ready_queue)
    {
        ready_queue = task;
        task->next = task;
    }
    else
    {
        task->next = ready_queue->next;
        ready_queue->next = task;
    }


    return task;
}





//Сборщик мусора. Очищает стек от задачи, у которой task->state == TASK_TERMINATED и убирает ее из списка задач
void cleanup_tasks(void)
{
    if(!ready_queue) return;
    task_t *cur = ready_queue;
    task_t *prev = NULL;

    do{
        task_t *next = cur->next;


        if(cur->pid != 0 &&
           cur->state == TASK_TERMINATED){
            // Сначала освобождаем всю пользовательскую память процесса
            vm_pages_free_all(cur);
            //Освобождаем kernel stack
            if(cur->kernel_stack_phys){
                pmm_free_page( (void*)cur->kernel_stack_phys );
                cur->kernel_stack_phys = 0;
                cur->kernel_stack = 0;
            }

            if(cur == ready_queue) {
                if(cur->next == cur) {
                    ready_queue = NULL;
                }
                else {
                    ready_queue = next;
                    task_t *tail = next;
                    while(tail->next != cur) tail = tail->next;
                    tail->next = next;
                }
            }
            else{
                prev->next = next;
            }

            kfree(cur);

            cur = next;
            if(!ready_queue) break;
            continue;
        }


        prev = cur;
        cur = next;


    } while(cur != ready_queue);
}





static uint32_t create_user_stack(task_t *task)
{
    uint32_t virt = 0x80000000;

    uint32_t phys=(uint32_t)pmm_alloc_page();

    if(!phys)
    {
        draw_string(10,300,"NO USER STACK");
        for(;;);
    }

    int32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;

    vmm_map_page(
        task->page_dir,
        virt,
        phys,
        flags
    );

    // Регистрируем stack page
    if(!vm_page_add( task, virt, phys, flags)) {
            draw_string(10,300,"NO VM STACK");
            for(;;);
    }


    task->user_stack_phys=phys;


    return virt + PAGE_SIZE;
}




task_t* get_first_task(void){
    if(!ready_queue) return NULL;
    return ready_queue;
}





int user_task_count(void){
    if (!ready_queue) return 0;
    int count = 0;
    task_t *t = ready_queue;
    do{
        if (t->pid != 0)
		count++;
        t = t->next;
    } while (t != ready_queue);
    return count;
}




// ============================================================
// Менеджер страниц виртуальной памяти процесса
// ============================================================

int vm_page_add(task_t *task,uint32_t virt,uint32_t phys,uint32_t flags)
{
    if(!task) return 0;

    vm_page_t *page = (vm_page_t*)kmalloc(sizeof(vm_page_t));

    if(!page) return 0;

    page->virt  = virt;
    page->phys  = phys;
    page->flags = flags;

    page->next = task->vm_pages;
    task->vm_pages = page;

    return 1;
}


// Удалить запись о странице из списка task.
// Физическую страницу здесь НЕ освобождаем.
int vm_page_remove(task_t *task,uint32_t virt)
{
    if(!task) return 0;

    vm_page_t *cur = task->vm_pages;
    vm_page_t *prev = NULL;

    while(cur){
        if(cur->virt == virt)
        {
            if(prev)
                prev->next = cur->next;
            else
                task->vm_pages = cur->next;

            return 1;
        }

        prev = cur;
        cur = cur->next;
    }

    return 0;
}


// Освободить все физические страницы,
// принадлежащие пользовательскому процессу.
void vm_pages_free_all(task_t *task)
{
    if(!task)
        return;

    vm_page_t *cur = task->vm_pages;

    draw_hex_dword(cur->virt, 500, 300);
    draw_hex_dword(cur->phys, 600, 300);

    while(cur)
    {
        vm_page_t *next = cur->next;

        // Сначала убираем виртуальное отображение.
        vmm_unmap_page(
            task->page_dir,
            cur->virt
        );

        // Затем возвращаем физическую страницу PMM.
        if(cur->phys) pmm_free_page((void*)cur->phys);

        kfree(cur);

        cur = next;
    }

    task->vm_pages = NULL;
}


