#include "../include/umemory.h"
#include "../include/ustd.h"
#include "../include/syscall.h"
#include <stddef.h>
#include <stdint.h>

static u_mem_block_t *u_free_list = NULL;



void* u_alloc_page(void){
    uint32_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_ALLOC_PAGE) // Номер вызова SYS_ALLOC_PAGE
        : "memory"
    );
    return (void*)ret;
}


int u_free_page(void *addr){
    int ret;
    // Используем константу SYS_FREE_PAGE через встроенный макрос,
    // сохраняя ручную атомарную загрузку регистров!
    __asm__ __volatile__(
        "movl %2, %%eax\n\t"  // Загружаем номер системного вызова (SYS_FREE_PAGE)
        "movl %1, %%ebx\n\t"  // Загружаем адрес страницы в целевой EBX
        "int $0x80"
        : "=a"(ret)
        : "r"(addr), "i"(SYS_FREE_PAGE)
        : "memory", "ebx"
    );
    return ret;
}


/*
//Старый вариант
void* malloc(size_t size){
    if (size == 0) return NULL;
    //Выравниваем размер под 4 байта
    size = (size + 3) & ~3;
    u_mem_block_t *prev = NULL;
    u_mem_block_t *cur = u_free_list;
    //Проверяем список повторного использования блоков
    while (cur) {
        if (cur->size >= size) {
            if (prev) prev->next = cur->next;
            else u_free_list = cur->next;

            // Вместо арифметики указателей возвращаем чистый явный адрес данных за заголовком
            uint32_t data_addr = (uint32_t)cur + sizeof(u_mem_block_t);
            return (void*)data_addr;
        }
        prev = cur;
        cur = cur->next;
    }
    //Округляем размер полезного графического буфера до целых физических страниц 4096 байт
    size_t aligned_user_size = (size + 4095) & ~4095;
    //Полный объем ОЗУ, запрашиваемый у sbrk: размер дескриптора + выровненный буфер
    size_t total_allocated_size = sizeof(u_mem_block_t) + aligned_user_size;
    //Вызываем sbrk. Переменная allocated_region гарантированно получит адрес вроде 0x50000000
    void *allocated_region = u_sbrk(total_allocated_size);
    if (!allocated_region || (uint32_t)allocated_region == 0) {
        return NULL;
    }
    //Принудительно интерпретируем начало региона как беззнаковый адрес структуры
    uint32_t block_base_addr = (uint32_t)allocated_region;
    u_mem_block_t *block = (u_mem_block_t*)block_base_addr;
    block->size = aligned_user_size;
    block->next = NULL;
    uint32_t final_user_data_pointer = block_base_addr + sizeof(u_mem_block_t);
    //Возвращаем кристально чистый, беззнаковый адрес (например, строго 0x50000008)
    return (void*)final_user_data_pointer;
}
*/

//Специальный транзакционный аллокатор для тяжелых массивов
void* malloc(size_t size){
    if (size == 0) return NULL;
    uint32_t pages_count = (size + 4095) / 4096;
    //Выделяем стартовую страницу кучи
    void *first_page = u_alloc_page();
    if (!first_page || (uint32_t)first_page == 0) {
        return NULL;
    }
    //Запускаем поштучный запрос остальных страниц
    for (uint32_t i = 1; i < pages_count; i++){
        void *next_page = u_alloc_page();
        //printf("i=%d next_page: %x\n",i,(uint32_t)next_page);
        uint32_t check_addr = (uint32_t)next_page;
        if (check_addr == 0) {
            free_pages(first_page, i * 4096);
            return NULL;
        }
    }
    return first_page;
}


int get_mem_info(am_mem_info_t *info){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_GET_MEM_INFO), "b"(info) : "memory");
    return ret;
}

void free(void *ptr){
    if (!ptr) return;
    u_mem_block_t *block = ((u_mem_block_t*)ptr) - 1;
    //Спокойно возвращаем блок в локальный список свободной памяти процесса
    block->next = u_free_list;
    u_free_list = block;
}

void free_pages(void *ptr, size_t size){
    if (!ptr || size == 0) return;
    //Вычисляем, сколько страниц ОЗУ занимает этот буфер
    uint32_t pages_count = (size + 4095) / 4096;
    uint32_t start_virt = (uint32_t)ptr;
    //Поочередно отдаем каждую виртуальную страницу обратно ядру через SYS_FREE_PAGE
    //Ядро сотрет маппинг в VMM и вернет физические блоки в PMM
    for (uint32_t i = 0; i < pages_count; i++) {
        uint32_t current_page_addr = start_virt + (i * PAGE_SIZE);
        u_free_page((void*)current_page_addr);
    }
}

void* u_sbrk(int32_t increment){
    uint32_t ret;
    __asm__ __volatile__(
        "movl %2, %%eax\n\t"
        "movl %1, %%ebx\n\t"
        "int $0x80"
        : "=a"(ret)
        : "r"(increment), "i"(SYS_SBRK)
        : "memory", "ebx"
    );
    return (void*)ret;
}
