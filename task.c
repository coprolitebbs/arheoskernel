#include "include-kernel/task.h"
#include <stdint.h>
#include <stddef.h>
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/lib.h"
#include "include-kernel/gdt.h"
#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"
#include "include-kernel/isr.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/vfs.h"
#include "include-kernel/spinlock.h"
#include "include-kernel/use_drivers.h"
#include "include-kernel/syscalls.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/config.h"
#include "include-kernel/elf32.h"

static spinlock_t sched_spinlock = {0};

int x_of = 8;
int shedule_count = 0;
volatile uint32_t idle_counter = 0;

//Текущая задача и очередь
task_t *current_task = NULL;
task_t *ready_queue = NULL;

//IDLE-задача с PID=0
static task_t idle_task;
static uint32_t idle_stack[1024];

static uint32_t next_pid = 1;
static uint32_t next_user_stack=0x800000;

//Инициализация планировщика
void init_tasking(void){
    current_task=(task_t*)kmalloc(sizeof(task_t));
    if(!current_task){
        draw_string(10,120,"NO CURRENT TASK");
        for(;;);
    }
    memset(current_task,0,sizeof(task_t));
    strcpy(current_task->cwd, "/");
    idle_task.pid = 0;
	idle_task.state = TASK_IDLE;
	idle_task.kernel_stack = (uint32_t)idle_stack + sizeof(idle_stack);
	idle_task.page_dir = kernel_directory;
	idle_task.page_dir_phys = (uint32_t)kernel_directory;
	idle_task.user_stack = 0;
	idle_task.next = 0;
	prepare_task_stack(&idle_task,(uint32_t)idle_loop);
}

void load_user_program(task_t *task,uint32_t virt_addr,uint8_t *program,uint32_t size){
    uint32_t pages=(size+PAGE_SIZE-1)/PAGE_SIZE;
    for(uint32_t i=0;i<pages;i++){
        uint32_t phys=(uint32_t)pmm_alloc_page();
        if(!phys){
            draw_string(10,600,"NO USER MEM");
            for(;;);
        }

        memset((void*)phys,0,PAGE_SIZE);

        uint32_t virt = virt_addr + i * PAGE_SIZE;
        vmm_map_page(task->page_dir, virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        if(!vm_page_add(task, virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER)){
                draw_string(10,600,"NO VM PAGE");
                for(;;);
        }

        uint32_t copy=size-i*PAGE_SIZE;
        if(copy>PAGE_SIZE) copy=PAGE_SIZE;
        memcpy((void*)phys,program+i*PAGE_SIZE,copy);
    }
    //task->heap_next = virt_addr + pages * PAGE_SIZE;
}

//Планировщик (вызывается из обработчика таймера)
void schedule(struct regs *r){
    task_t *old_task = current_task;
    //Сохраняем контекст регистров текущей выполняемой задачи
    if(old_task && old_task->pid != 0){
        if(old_task->state == TASK_RUNNING || old_task->state == TASK_BLOCKED){
            if (r != NULL) old_task->esp = (uint32_t)r;
            if(old_task->state == TASK_RUNNING){
                old_task->state = TASK_READY;
            }
        }
        else if(old_task->state == TASK_TERMINATED){
            old_task->esp = 0;
        }
    }

    //Вызываем сборщик мусора ядра
    cleanup_tasks();

    //Ищем следующую готовую задачу Ring 3 в кольце очереди
    task_t *next_task = NULL;
    if (ready_queue){
        task_t *start = (old_task && old_task->state != TASK_TERMINATED && old_task->pid != 0) ? old_task->next : ready_queue;
        task_t *t = start;

        do {
            if (t->state == TASK_READY) {
                next_task = t;
                break;
            }
            t = t->next;
        } while (t != start);
    }

    //Если готовых пользовательских задач нет — уходим в системный idle_task (PID=0)
    if(!next_task){
        next_task = &idle_task;
    }

    if (next_task == old_task && old_task->state != TASK_TERMINATED) {
        current_task->state = TASK_RUNNING;
        return;
    }
    uint32_t target_esp = next_task->esp;

    //Переключаем контекст адресных пространств (CR3) для новой задачи
    current_task = next_task;
    current_task->state = TASK_RUNNING;
    update_tss_esp0(current_task->kernel_stack);

    vmm_switch_directory(current_task->page_dir_phys);

    uint32_t flush_cr3;
    __asm__ __volatile__(
        "mov %%cr3, %0\n\t"
        "mov %0, %%cr3"
        : "=r"(flush_cr3)
        :
        : "memory"
    );

    //Передаем в switch_task ЧИСТУЮ, изолированную локальную переменную target_esp,
    //исключая ложное чтение мусора из чужих структур и Page Fault / GPF
    switch_task(target_esp);

    for(;;);
}

void prepare_task_stack(task_t *task, uint32_t entry){
    uint32_t *kstack = (uint32_t*)task->kernel_stack;
    if(task->pid==0){
        //Укладываем фрейм возврата iret для режима ядра Ring 0
        *(--kstack) = 0x202;       //EFLAGS (IF=1, IOPL=0)
        *(--kstack) = 0x08;        //CS ядра (Селектор кода Ring 0)
        *(--kstack) = entry;       //EIP (Точка входа idle_loop)

        //Укладываем заглушки номера прерывания и кода ошибки
        *(--kstack) = 0;           //err_code dummy
        *(--kstack) = 0;           //int_no dummy

        //Укладываем pusha кадр общего назначения (8 регистров по 4 байта)
        *(--kstack) = 0; //eax
        *(--kstack) = 0; //ecx
        *(--kstack) = 0; //edx
        *(--kstack) = 0; //ebx
        *(--kstack) = 0; //esp dummy
        *(--kstack) = 0; //ebp
        *(--kstack) = 0; //esi
        *(--kstack) = 0; //edi

        //Укладываем легальные системные селекторы данных ядра (0x10)
        //Это зафиксирует ESP ядра внутри безопасных границ массива idle_stack
        *(--kstack) = 0x10;        //ds ядра
        *(--kstack) = 0x10;        //es ядра
        *(--kstack) = 0x10;        //fs ядра
        *(--kstack) = 0x10;        //gs ядра

        task->esp = (uint32_t)kstack;
        return;
    }
    //Стек Ring 3 для switch_task процессов пользователя
    *(--kstack) = 0x23;              //SS user data
    *(--kstack) = task->user_stack;  //ESP (Уже содержит argc и argv, подготовленные ниже)
    *(--kstack) = 0x200;             //EFLAGS
    *(--kstack) = 0x1B;              //CS user code
    *(--kstack) = entry;             //EIP

    *(--kstack)=0;
    *(--kstack)=0;

    *(--kstack)=0; //eax
    *(--kstack)=0; //ecx
    *(--kstack)=0; //edx
    *(--kstack)=0; //ebx
    *(--kstack)=0; //esp dummy
    *(--kstack)=0; //ebp
    *(--kstack)=0; //esi
    *(--kstack)=0; //edi

    *(--kstack)=0x23; //ds
    *(--kstack)=0x23; //es
    *(--kstack)=0x23; //fs
    *(--kstack)=0x23; //gs
    task->esp=(uint32_t)kstack;
}


task_t* create_task(uint32_t entry){
    //kernel_schedule_lock = 1;
    task_t *task = (task_t*)kmalloc(sizeof(task_t));
    if(!task) for(;;);
    memset(task,0,sizeof(task_t));

    //Инициализация STDIN STDOUT STDERR
    task->fds[0] = 0;  // Локальный STDIN (Клавиатура TTY) -> Глобальный 0
    task->fds[1] = 1;  // Локальный STDOUT (Экран Видео)  -> Глобальный 1
    task->fds[2] = 2;  // Локальный STDERR (Лог Ошибок)   -> Глобальный 2
    //Все остальные дескрипторы по умолчанию закрыты
    for (int i = 3; i < MAX_PROCESS_FDS; i++){
        task->fds[i] = -1;
    }
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
    task->heap_next = 0x50000000;//0x40001000;
    //task->heap_next = virt_addr + pages * PAGE_SIZE;
    uint32_t pd_phys;
    task->page_dir = vmm_create_address_space(&pd_phys);
    task->page_dir_phys = pd_phys;
    if(!task->page_dir) for(;;);
    task->user_stack = create_user_stack(task);
    //Формируем искусственный стек, как будто задача была остановлена внутри isr_common
    prepare_task_stack(task, entry);
    //Добавляем в очередь
    if(!ready_queue){
        ready_queue = task;
        task->next = task;
    } else {
        task->next = ready_queue->next;
        ready_queue->next = task;
    }

    return task;
}


//Сборщик мусора. Очищает стек от задачи, у которой task->state == TASK_TERMINATED и убирает ее из списка задач
void cleanup_tasks(void){
    if(!ready_queue) return;
    task_t *cur = ready_queue;
    task_t *prev = NULL;
    do{
        task_t *next = cur->next;
        if(cur->pid != 0 && cur->state == TASK_TERMINATED){
            //Сначала освобождаем всю пользовательскую память процесса
            vm_pages_free_all(cur);

            //Освобождаем kernel stack
            if(cur->kernel_stack_phys){
                pmm_free_page( (void*)cur->kernel_stack_phys );
                cur->kernel_stack_phys = 0;
                cur->kernel_stack = 0;
            }
            if(cur == ready_queue){
                if(cur->next == cur){
                    ready_queue = NULL;
                } else {
                    ready_queue = next;
                    task_t *tail = next;
                    while(tail->next != cur) tail = tail->next;
                    tail->next = next;
                }
            } else {
                prev->next = next;
            }
            uint32_t sync_tlb_cr3;
            __asm__ __volatile__(
                "mov %%cr3, %0\n\t"
                "mov %0, %%cr3"
                : "=r"(sync_tlb_cr3)
                :
                : "memory"
            );

            kfree(cur);
            cur = next;
            if(!ready_queue) break;
            continue;
        }
        prev = cur;
        cur = next;
    } while(cur != ready_queue);
}

static uint32_t create_user_stack(task_t *task){
    //Сдвигаем виртуальный адрес стека Ring 3 на одну страницу вниз
    //Теперь страница будет полностью покрывать диапазон от 0x7FFFF000 до 0x7FFFFFFF
    uint32_t virt = 0x80000000 - PAGE_SIZE;
    uint32_t phys=(uint32_t)pmm_alloc_page();
    if(!phys){
        draw_string(10,300,"NO USER STACK");
        for(;;);
    }
    int32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    vmm_map_page(task->page_dir,virt,phys,flags);

    if(!vm_page_add( task, virt, phys, flags)){
            draw_string(10,300,"NO VM STACK");
            for(;;);
    }
    task->user_stack_phys=phys;

    //Возвращаем физическую вершину стека (конец замапленной страницы 0x7FFFFFFF)
    return virt + PAGE_SIZE;
}



task_t* get_first_task(void){
    if(!ready_queue) return &idle_task;//NULL;
    return ready_queue;
}


int user_task_count(void){
    if (!ready_queue) return 0;
    int count = 0;
    task_t *t = ready_queue;
    do{
        if (t->pid != 0) count++;
        t = t->next;
    } while (t != ready_queue);
    return count;
}

//Менеджер страниц виртуальной памяти процесса
int vm_page_add(task_t *task,uint32_t virt,uint32_t phys,uint32_t flags){
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


//Удалить запись о странице из списка task, физическую страницу здесь не освобождаем.
int vm_page_remove(task_t *task,uint32_t virt){
    if(!task) return 0;
    vm_page_t *cur = task->vm_pages;
    vm_page_t *prev = NULL;
    while(cur){
        if(cur->virt == virt){
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


//Освободить все физические страницы, принадлежащие пользовательскому процессу
void vm_pages_free_all(task_t *task){
    if(!task) return;
    vm_page_t *cur = task->vm_pages;
    while(cur){
        vm_page_t *next = cur->next;
        //Убираем виртуальное отображение из таблиц страниц этого процесса
        vmm_unmap_page(task->page_dir, cur->virt);
        //Возвращаем физическую страницу менеджеру PMM только если это
        //реальная оперативная память пользовательского процесса Ring 3.
        //Если физический адрес cur->phys указывает на область выше 128 МБ (0x08000000),
        //или равен адресу LFB (0xFD22B000), ядру запрещено
        //стирать его из PMM, иначе это уничтожит видеоадаптер и вызовет Page Fault
        if (cur->phys && (uint32_t)cur->phys < 0x08000000) {
            pmm_free_page((void*)cur->phys);
        }

        kfree(cur);
        cur = next;
    }
    task->vm_pages = NULL;
}



task_t* task_spawn_builtin(uint32_t entry_virt, uint8_t *program_code, uint32_t program_size, const char *cmdline){
    task_t *task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) return NULL;
    memset(task, 0, sizeof(task_t));

    task->fds[0] = 0; task->fds[1] = 1; task->fds[2] = 2;
    for (int i = 3; i < MAX_PROCESS_FDS; i++) task->fds[i] = -1;

    task->kernel_stack_phys = (uint32_t)pmm_alloc_page();
    if (!task->kernel_stack_phys){ kfree(task); return NULL; }
    memset((void*)task->kernel_stack_phys, 0, PAGE_SIZE);
    task->kernel_stack = task->kernel_stack_phys + PAGE_SIZE;

    task->pid = next_pid++;
    task->state = TASK_READY;
    task->parameter = 0;
    task->heap_next = 0x50000000;//0x40001000;
    //task->heap_next = virt_addr + pages * PAGE_SIZE;

    uint32_t pd_phys;
    task->page_dir = vmm_create_address_space(&pd_phys);
    task->page_dir_phys = pd_phys;
    if (!task->page_dir){ pmm_free_page((void*)task->kernel_stack_phys); kfree(task); return NULL; }

    task->user_stack = create_user_stack(task);
    load_user_program(task, entry_virt, program_code, program_size);

    uint32_t old_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(task->page_dir_phys));

    uint32_t ustack_top = task->user_stack;

    char (*args_tokens)[64] = (char (*)[64])kmalloc(16 * 64);
    if (!args_tokens) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3));
        //В продакшене здесь нужна полная зачистка, пока упростим для стабильности
        return NULL;
    }
    memset(args_tokens, 0, 16 * 64);

    int argc = 0;

    if (cmdline && cmdline[0] != '\0'){
        int t_idx = 0;
        for (int i = 0; cmdline[i] != '\0' && argc < 16; i++){
            if (cmdline[i] == ' ' || cmdline[i] == '\t'){
                if (t_idx > 0) {
                    args_tokens[argc][t_idx] = '\0';
                    argc++;
                    t_idx = 0;
                }
            } else {
                if (t_idx < 63){
                    args_tokens[argc][t_idx++] = cmdline[i];
                }
            }
        }
        if (t_idx > 0 && argc < 16){
            args_tokens[argc][t_idx] = '\0';
            argc++;
        }
    } else {
        strcpy(args_tokens[0], "unknown");
        argc = 1;
    }

    uint32_t argv_addresses[16];
    for (int i = argc - 1; i >= 0; i--){
        int len = strlen(args_tokens[i]) + 1;
        ustack_top -= len;
        memcpy((void*)ustack_top, args_tokens[i], len);
        argv_addresses[i] = ustack_top;
    }

    ustack_top &= ~3;

    ustack_top -= sizeof(uint32_t);
    *(uint32_t*)ustack_top = 0;

    for (int i = argc - 1; i >= 0; i--){
        ustack_top -= sizeof(uint32_t);
        *(uint32_t*)ustack_top = argv_addresses[i];
    }

    uint32_t argv_array_ptr = ustack_top;

    ustack_top -= sizeof(uint32_t);
    *(uint32_t*)ustack_top = argc;

    task->user_stack = ustack_top;

    __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3));

    //Освобождаем изолированный буфер ядра, он больше не нужен, так как данные уже на стеке Ring 3
    kfree(args_tokens);

    if (current_task && current_task->pid != 0) {
        strcpy(task->cwd, current_task->cwd);
    } else {
        strcpy(task->cwd, "/"); //Страховка при первом старте
    }

    prepare_task_stack(task, entry_virt);

    uint32_t eflags = spin_lock_irqsave(&sched_spinlock);
    if (!ready_queue){ ready_queue = task; task->next = task; }
    else { task->next = ready_queue->next; ready_queue->next = task; }
    spin_unlock_irqrestore(&sched_spinlock, eflags);

    return task;
}




// Функция парсинга и загрузки исполняемого ELF-файла в пользовательское пространство (Ring 3)
task_t* task_spawn_elf(const char *path, uint8_t *elf_image, uint32_t size, const char *cmdline){
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_image;

    // Базовый адрес (Delta) для релокации PIE-исполняемого файла
    // Если файл статический (ehdr->type == ET_EXEC), load_base будет 0.
    // Если файл позиционно-независимый (ehdr->type == ET_DYN), сажаем на 0x40000000.
    uint32_t load_base = (ehdr->type == 3) ? 0x40000000 : 0; // ET_DYN = 3

    task_t *task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) return NULL;
    memset(task, 0, sizeof(task_t));

    task->fds[0] = 0; task->fds[1] = 1; task->fds[2] = 2;
    for (int i = 3; i < MAX_PROCESS_FDS; i++) task->fds[i] = -1;

    task->kernel_stack_phys = (uint32_t)pmm_alloc_page();
    if (!task->kernel_stack_phys) { kfree(task); return NULL; }
    memset((void*)task->kernel_stack_phys, 0, PAGE_SIZE);
    task->kernel_stack = task->kernel_stack_phys + PAGE_SIZE;

    task->pid = next_pid++;
    task->state = TASK_READY;
    task->heap_next = 0x50000000;

    uint32_t pd_phys;
    task->page_dir = vmm_create_address_space(&pd_phys);
    task->page_dir_phys = pd_phys;
    if (!task->page_dir) { pmm_free_page((void*)task->kernel_stack_phys); kfree(task); return NULL; }

    task->user_stack = create_user_stack(task);

    // 1. ЗАГРУЗКА СЕГМЕНТОВ PT_LOAD С УЧЕТОМ LOAD_BASE
    Elf32_Phdr *phdr = (Elf32_Phdr*)(elf_image + ehdr->phoff);
    Elf32_Shdr *sections = (Elf32_Shdr*)(elf_image + ehdr->shoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type == 1) { // PT_LOAD
            // Применяем смещение load_base к виртуальному адресу сегмента
            uint32_t vaddr = phdr[i].vaddr + load_base;
            uint32_t memsize = phdr[i].memsz;
            uint32_t filesize = phdr[i].filesz;
            uint32_t offset = phdr[i].offset;

            uint32_t start_page = vaddr & ~0xFFF;
            uint32_t end_page = (vaddr + memsize + PAGE_SIZE - 1) & ~0xFFF;
            uint32_t pages_needed = (end_page - start_page) / PAGE_SIZE;

            for (uint32_t p = 0; p < pages_needed; p++) {
                uint32_t page_vaddr = start_page + p * PAGE_SIZE;

                if (vmm_get_phys_addr(task->page_dir, page_vaddr) == 0) {
                    uint32_t phys = (uint32_t)pmm_alloc_page();
                    if (!phys) return NULL;
                    memset((void*)phys, 0, PAGE_SIZE);

                    uint32_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
                    vmm_map_page(task->page_dir, page_vaddr, phys, flags);
                    vm_page_add(task, page_vaddr, phys, flags);
                }
            }

            uint32_t old_cr3;
            __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
            __asm__ __volatile__("mov %0, %%cr3" : : "r"(task->page_dir_phys));

            if (filesize > 0) {
                memcpy((void*)vaddr, elf_image + offset, filesize);
            }
            if (memsize > filesize) {
                memset((void*)(vaddr + filesize), 0, memsize - filesize);
            }

            __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3));
        }
    }

    // 2. ДИНАМИЧЕСКАЯ РЕЛОКАЦИЯ ПЕРЕМЕННЫХ И СТРОК (PIE ДИНАМИЧЕСКИЙ ЛИНКОВЩИК)
    // Ищем релокационные таблицы в заголовках секций ELF-образа
    if (load_base > 0) {
        for (int i = 0; i < ehdr->shnum; i++) {
            // Ищем секции релокаций (SHT_REL == 9)
            if (sections[i].type == 9 || (sections[i].type == 1 && sections[i].entsize == sizeof(Elf32_Rel))) {
                Elf32_Rel *rels = (Elf32_Rel*)(elf_image + sections[i].offset);
                int num_relocs = sections[i].size / sizeof(Elf32_Rel);

                uint32_t old_cr3;
                __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
                __asm__ __volatile__("mov %0, %%cr3" : : "r"(task->page_dir_phys));

                for (int r = 0; r < num_relocs; r++) {
                    uint32_t type = ELF32_R_TYPE(rels[r].info);

                    // Нас интересует базовый тип относительного смещения PIE-компилятора
                    if (type == R_386_RELATIVE || type == 1) { // R_386_32 / R_386_RELATIVE
                        // Вычисляем адрес, куда указывает релокация, в памяти процесса
                        uint32_t *patch_ptr = (uint32_t*)(rels[r].offset + load_base);

                        // Прибавляем смещение load_base (0x40000000) к значению по этому адресу
                        *patch_ptr += load_base;
                    }
                }

                __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3));
            }
        }
    }

    // 3. ПОДГОТОВКА СТЕКА И АРГУМЕНТОВ (Оставляем старую рабочую схему)
    uint32_t old_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(task->page_dir_phys));

    uint32_t ustack_top = task->user_stack;
    char (*args_tokens)[64] = (char (*)[64])kmalloc(16 * 64);
    if (!args_tokens) { __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3)); return NULL; }
    memset(args_tokens, 0, 16 * 64);

    int argc = 0;
    const char *prog_name = path;
    for (int i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') prog_name = &path[i + 1];
    }
    if (*prog_name == '\0') prog_name = "unknown";

    strcpy(args_tokens[0], prog_name);
    argc = 1;

    if (cmdline && cmdline[0] != '\0'){
        int t_idx = 0;
        for (int i = 0; cmdline[i] != '\0' && argc < 16; i++){
            if (cmdline[i] == ' ' || cmdline[i] == '\t'){
                if (t_idx > 0) { args_tokens[argc][t_idx] = '\0'; argc++; t_idx = 0; }
            } else {
                if (t_idx < 63) args_tokens[argc][t_idx++] = cmdline[i];
            }
        }
        if (t_idx > 0 && argc < 16) { args_tokens[argc][t_idx] = '\0'; argc++; }
    }

    uint32_t argv_addresses[16];
    for (int i = argc - 1; i >= 0; i--){
        int len = strlen(args_tokens[i]) + 1;
        ustack_top -= len;
        memcpy((void*)ustack_top, args_tokens[i], len);
        argv_addresses[i] = ustack_top;
    }

    ustack_top &= ~3;
    ustack_top -= sizeof(uint32_t);
    *(uint32_t*)ustack_top = 0;

    for (int i = argc - 1; i >= 0; i--){
        ustack_top -= sizeof(uint32_t);
        *(uint32_t*)ustack_top = argv_addresses[i];
    }

    ustack_top -= sizeof(uint32_t);
    *(uint32_t*)ustack_top = argc;

    task->user_stack = ustack_top;
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(old_cr3));
    kfree(args_tokens);

    if (current_task && current_task->pid != 0) strcpy(task->cwd, current_task->cwd);
    else strcpy(task->cwd, "/");

    // Критический фикс: Точка входа также смещается на load_base!
    uint32_t entry_point = ehdr->entry + load_base;
    prepare_task_stack(task, entry_point);

    uint32_t eflags = spin_lock_irqsave(&sched_spinlock);
    if (!ready_queue){ ready_queue = task; task->next = task; }
    else { task->next = ready_queue->next; ready_queue->next = task; }
    spin_unlock_irqrestore(&sched_spinlock, eflags);

    return task;
}






task_t *task_spawn_from_file(const char *path, uint32_t entry_virt, const char *cmdline){
    if (!path) return NULL;

    fs_stat_t st;
    memset(&st, 0, sizeof(fs_stat_t));

    int stat_res = vfs_stat(path, &st);
    if (stat_res != VFS_STATUS_OK){
        //com_puts("[KERNEL ERROR] task_spawn: Cannot stat file: "); com_puts(path); com_puts("\n");
        return NULL;
    }

    uint32_t file_size = st.size;
    if (file_size == 0 || file_size > (1024 * 1024)){
        //com_puts("[KERNEL ERROR] task_spawn: Executable size is invalid or too large\n");
        return NULL;
    }

    int fd = vfs_open(path, FS_OPEN_READ);
    if (fd < 0){
        //com_puts("[KERNEL ERROR] task_spawn: Cannot open file: "); com_puts(path); com_puts("\n");
        return NULL;
    }

    uint8_t *kernel_load_buf = (uint8_t *)kmalloc(file_size);
    if (!kernel_load_buf){ vfs_close(fd); return NULL; }
    memset(kernel_load_buf, 0, file_size);

    //Поблочно считываем весь исполняемый файл с диска в кучу ядра (фикс обрезания хвостов)
    uint32_t total_bytes_read = 0;
    bool read_error = false;
    while (total_bytes_read < file_size){
        uint32_t bytes_to_read = file_size - total_bytes_read;
        if (bytes_to_read > 512) bytes_to_read = 512;

        int r = vfs_read(fd, kernel_load_buf + total_bytes_read, bytes_to_read);
        if (r <= 0){
            read_error = true;
            break;
        }
        total_bytes_read += r;
    }
    vfs_close(fd);

    if (read_error || total_bytes_read < file_size){
        kfree(kernel_load_buf);
        //com_puts("[KERNEL ERROR] task_spawn: Executable read error or truncated\n");
        return NULL;
    }
    //Автоопределение формата по магическому числу
    uint8_t *magic = kernel_load_buf;
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F'){
        //Обнаружен elf
        //com_puts("[KERNEL] Format match: ELF executable detected. Loading headers...\n");
        task_t *elf_task = task_spawn_elf(path, kernel_load_buf, total_bytes_read, cmdline);
        kfree(kernel_load_buf);
        return elf_task;
    } else {
        //Обычный FLAT-бинарник разметки 0x40000000
        //com_puts("[KERNEL] Format match: Flat binary detected. Loading to default text section...\n");
        task_t *flat_task = task_spawn_builtin(entry_virt, kernel_load_buf, total_bytes_read, cmdline);
        kfree(kernel_load_buf);
        return flat_task;
    }
}





task_t* create_kernel_thread(uint32_t entry_virt){
    //Аллоцируем память под структуру задачи
    task_t *task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) return NULL;
    memset(task, 0, sizeof(task_t));
    //Выделяем личную физическую страницу под стек этого потока ядра
    task->kernel_stack_phys = (uint32_t)pmm_alloc_page();

    if (!task->kernel_stack_phys) {
        kfree(task);
        return NULL;
    }
    memset((void*)task->kernel_stack_phys, 0, PAGE_SIZE);
    task->kernel_stack = task->kernel_stack_phys + PAGE_SIZE; //Стек растет вниз

    task->pid = next_pid++;
    task->state = TASK_READY;
    task->type = TASK_DRIVER; //Системный поток ядра
    task->parameter = 0;

    //Системный поток делит таблицы страниц ядра (CR3) со всей ОС
    task->page_dir = kernel_directory;
    task->page_dir_phys = (uint32_t)kernel_directory;
    task->user_stack = 0;
    task->heap_next = 0x50000000;//0x40001000;

    uint32_t *kstack = (uint32_t*)task->kernel_stack;

    //Аппаратный фрейм инструкции IRET для возврата процессора в Ring 0:
    *(--kstack) = 0x202;       //EFLAGS (IF=1, IOPL=0)
    *(--kstack) = 0x08;        //CS ядра (Селектор кода ядра)
    *(--kstack) = entry_virt;  //EIP (Точка входа в kernel_init_thread)

    //Имитируем пуши заглушек прерывания из isr.asm:
    *(--kstack) = 0;           //err_code
    *(--kstack) = 0;           //int_no

    //Имитируем pusha диспетчера:
    *(--kstack) = 0;           // eax
    *(--kstack) = 0;           // ecx
    *(--kstack) = 0;           // edx
    *(--kstack) = 0;           // ebx
    *(--kstack) = 0;           // esp dummy
    *(--kstack) = 0;           // ebp
    *(--kstack) = 0;           // esi
    *(--kstack) = 0;           // edi

    //Имитируем пуши сегментных регистров ядра из isr_common (0x10 - данные ядра):
    *(--kstack) = 0x10;        // ds
    *(--kstack) = 0x10;        // es
    *(--kstack) = 0x10;        // fs
    *(--kstack) = 0x10;        // gs

    //Фиксируем получившийся чистейший указатель вершины стека в дескрипторе задачи!
    task->esp = (uint32_t)kstack;

    //Атомарно врезаем новый поток в кольцевую очередь планировщика
    uint32_t eflags = spin_lock_irqsave(&sched_spinlock);

    if (!ready_queue){
        ready_queue = task;
        task->next = task;
    } else {
        task->next = ready_queue->next;
        ready_queue->next = task;
    }
    spin_unlock_irqrestore(&sched_spinlock, eflags);

    return task;
}



void kernel_init_thread(void){
    drivers_init();
    kconsole_init();

    __asm__ __volatile__("sti" ::: "memory");

    com_puts("[KERNEL THREAD] System Init Thread started successfully!\n");
    //Инициализация драйверов дисковода и VFS
    //Планировщик в этот момент работает, прерывание 32 тикает фоном
    //Драйвер ФС сам переключает свой локальный schedule_lock на время чтения портов
    int r = vfs_init_system();
    //Выводим статус монтирования на COM-порт или дебаг-область
    com_puts("[KERNEL THREAD] VFS Init Status: ");
    com_puthex(r);
    com_puts("\n");

    if(r == VFS_STATUS_OK){
        cfg_file_t *cfg = cfg_open("/config/boot/boot.cfg", FS_OPEN_READ,true);

        kconsole_reconfigure(cfg);
        int adddrv_status = load_additional_drivers(cfg);
        if(adddrv_status != DRV_STATUS_OK){
            com_puts("[KERNEL THREAD] Additional drv load error: ");
            com_puthex(adddrv_status);
            com_puts("\n");
        }
        cfg_close(cfg);
    }

    //task_spawn_from_file("/usbin/hello.bin", 0x40000000,"-v --test-mode");
    task_spawn_from_file("/bin/horus.bin", 0x40000000, NULL);
    //task_spawn_from_file("/usbin/vfs_test.bin", 0x40000000, NULL);
    //task_spawn_from_file("/usbin/cat.bin", 0x40000000, NULL);
    //task_spawn_from_file("/usbin/hello.bin", 0x40000000, NULL);
    //task_spawn_from_file("/usbin/kb_check.bin", 0x40000000, NULL);
    //task_spawn_from_file("/usbin/allocram.bin", 0x40000000, "512");

    task_spawn_from_file("/bin/sekhmet.bin", 0x40000000, NULL);


    //current_task = ready_queue;


    com_puts("[KERNEL THREAD] Userspace task HELLO.BIN spawned from disk.\n");

    com_puts("[KERNEL THREAD] Initialisation completed. Terminating init thread.\n");

    if(init_kernel_schedule_lock == 1){
        com_puts("[KERNEL THREAD] init_kernel_schedule_lock == 1\n");
    } else {
        com_puts("[KERNEL THREAD] init_kernel_schedule_lock == 0\n");
    }

    //Самоуничтожение потока инициализации
    //Переводим состояние текущего потока в TERMINATED и вызываем планировщик,
    //чтобы сборщик мусора очистил память этой страницы стека, а ядро переключилось
    //на выполнение загруженного процесса
    __asm__ __volatile__(
        "movl $" XSTR(SYS_EXIT) ", %%eax\n\t" // Подставит "movl $3, %eax"
        "int $0x80"
        : : : "memory"
    );

    //Сюда процессор больше никогда не вернется
    for(;;);
}




