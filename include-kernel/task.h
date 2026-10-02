#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "isr.h"
#include "vmm.h"
#include "vfs.h"


#define TASK_NORMAL 0
#define TASK_DRIVER 1

#define MAX_PROCESS_FDS 16

//Состояния
#define TASK_RUNNING  0
#define TASK_READY    1
#define TASK_BLOCKED  2
#define TASK_TERMINATED 3
#define TASK_IDLE 4

#define PATH_LEN_MAX 4096

extern int shedule_count;
extern int ready_queue_count;



//Переключение контекста (из switch.asm)
extern void switch_task(uint32_t new_esp);

//Структура задачи
typedef struct task{

    uint32_t pid;

    uint32_t state;

	uint32_t type;

	uint32_t parameter;

    struct task *next;

    page_directory_t *page_dir;

    uint32_t page_dir_phys;

    uint32_t esp;

    uint32_t user_stack;
	uint32_t user_stack_phys;

	uint32_t kernel_stack;
	uint32_t kernel_stack_phys;

	uint32_t heap_next;

	vm_page_t *vm_pages;

	uint32_t wait_reason;

	//Локальная таблица дескрипторов процесса (потоки)
	int fds[MAX_PROCESS_FDS];

	char cwd[PATH_LEN_MAX];

} task_t;

extern task_t *ready_queue;
extern task_t *current_task;

struct regs;

//Инициализация планировщика
void init_tasking(void);

//Загрузка пользовательской программы из массива uint8_t *program
void load_user_program(task_t *task,uint32_t virt_addr,uint8_t *program,uint32_t size);

//Планировщик (вызывается из обработчика таймера)
void schedule(struct regs *r);

//Создание новой задачи (entry = адрес функции или машинного кода)
task_t* create_task(uint32_t entry);

//Функция подготовки стека
void prepare_task_stack(task_t *task, uint32_t entry);

//Функция переключения контекста (внешняя, из switch.asm)
void switch_task(uint32_t new_esp);

//Сборщик мусора. Очищает стек от задачи, у которой task->state == TASK_TERMINATED и убирает ее из списка задач
void cleanup_tasks(void);

static uint32_t create_user_stack(task_t *task);

task_t* get_first_task(void);

int user_task_count(void);

//Менеджер страниц виртуальной памяти процесса
int vm_page_add(task_t *task,uint32_t virt,uint32_t phys,uint32_t flags);
int vm_page_remove(task_t *task,uint32_t virt);
void vm_pages_free_all(task_t *task);

//асинхронный запуск процессов
task_t* task_spawn_builtin(uint32_t entry_virt, uint8_t *program_code, uint32_t program_size, const char *cmdline);
task_t* task_spawn_from_file(const char *path, uint32_t entry_virt, const char *cmdline);
task_t* task_spawn_elf(const char *path, uint8_t *elf_image, uint32_t size, const char *cmdline);

task_t* create_kernel_thread(uint32_t entry_virt);
void kernel_init_thread(void);

#endif
