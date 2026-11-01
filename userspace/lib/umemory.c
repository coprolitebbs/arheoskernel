#include "../include/umemory.h"
#include "../include/ustd.h"
#include "../include/syscall.h"
#include <stddef.h>
#include <stdint.h>

/* ============ низкоуровневые обёртки над syscall ============ */

void* u_alloc_page(void){
    uint32_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_ALLOC_PAGE)
        : "memory"
    );
    return (void*)ret;
}

int u_free_page(void *addr){
    int ret;
    __asm__ __volatile__(
        "movl %2, %%eax\n\t"
        "movl %1, %%ebx\n\t"
        "int $0x80"
        : "=a"(ret)
        : "r"(addr), "i"(SYS_FREE_PAGE)
        : "memory", "ebx"
    );
    return ret;
}

int get_mem_info(am_mem_info_t *info){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_GET_MEM_INFO), "b"(info) : "memory");
    return ret;
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

/* ============ аллокатор ============ */

/*
 * Формат блока в памяти:
 *
 *     [ u_hdr_t | payload ... ]
 *      8 байт    size
 *
 * hdr.magic == SMALL_MAGIC  →  hdr.info = индекс класса slab
 *                              размер слота = 16 << hdr.info
 * hdr.magic == LARGE_MAGIC  →  hdr.info = сколько contiguous-страниц
 *
 * Для SMALL-пути базовые страницы держатся в slab-пулах до конца процесса
 * (переиспользуются через freelist). Для LARGE-пути страницы освобождаются
 * в PMM при free().
 */

#define SMALL_MAGIC  0x534D4C42u    /* "SMLB" */
#define LARGE_MAGIC  0x4C415247u    /* "LARG" */

#define NUM_CLASSES        8
#define SMALL_MAX_PAYLOAD  2040     /* верхняя граница small-запроса */

typedef struct {
    uint32_t magic;
    uint32_t info;
} u_hdr_t;

typedef struct u_slot {
    struct u_slot *next;
} u_slot_t;

/* список свободных слотов для каждого класса (индекс = класс) */
static u_slot_t *class_free_list[NUM_CLASSES] = {0};

static inline uint32_t slot_size_for(int cls){
    return 16u << cls;              /* 16, 32, 64, ... 2048 */
}

/* индекс класса для запроса size, или -1 если нужен LARGE */
static int class_for(uint32_t size){
    if (size > SMALL_MAX_PAYLOAD) return -1;
    uint32_t need = size + (uint32_t)sizeof(u_hdr_t);
    int cls = 0;
    while (slot_size_for(cls) < need) cls++;
    return cls;
}

/* забирает одну страницу у ядра и нарезает на слоты класса */
static void refill_class(int cls){
    uint8_t *page = (uint8_t *)u_alloc_page();
    if (!page) return;
    uint32_t ss = slot_size_for(cls);
    for (uint32_t off = 0; off + ss <= PAGE_SIZE; off += ss){
        u_slot_t *s = (u_slot_t *)(page + off);
        s->next = class_free_list[cls];
        class_free_list[cls] = s;
    }
}

void *malloc(size_t size){
    if (size == 0) return NULL;

    int cls = class_for((uint32_t)size);

    if (cls >= 0){
        /* ---------- SMALL ---------- */
        if (class_free_list[cls] == NULL){
            refill_class(cls);
            if (class_free_list[cls] == NULL) return NULL;   /* OOM */
        }
        u_slot_t *s = class_free_list[cls];
        class_free_list[cls] = s->next;

        u_hdr_t *h = (u_hdr_t *)s;
        h->magic = SMALL_MAGIC;
        h->info  = (uint32_t)cls;
        return (void *)(h + 1);
    }

    /* ---------- LARGE ---------- */
    uint32_t total = (uint32_t)size + (uint32_t)sizeof(u_hdr_t);
    uint32_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    uint8_t *base = NULL;
    for (uint32_t i = 0; i < pages; i++){
        void *p = u_alloc_page();
        if (!p){
            /* откат того, что уже выделили */
            if (base){
                for (uint32_t j = 0; j < i; j++)
                    u_free_page(base + j * PAGE_SIZE);
            }
            return NULL;
        }
        if (!base) base = (uint8_t *)p;
    }

    u_hdr_t *h = (u_hdr_t *)base;
    h->magic = LARGE_MAGIC;
    h->info  = pages;
    return (void *)(h + 1);
}

void free(void *ptr){
    if (!ptr) return;

    u_hdr_t *h = ((u_hdr_t *)ptr) - 1;

    if (h->magic == SMALL_MAGIC){
        uint32_t cls = h->info;
        if (cls >= NUM_CLASSES) return;            /* повреждено — игнорируем */
        u_slot_t *s = (u_slot_t *)h;
        s->next = class_free_list[cls];
        class_free_list[cls] = s;
        return;
    }

    if (h->magic == LARGE_MAGIC){
        uint32_t pages = h->info;
        uint8_t *base = (uint8_t *)h;
        for (uint32_t i = 0; i < pages; i++)
            u_free_page(base + i * PAGE_SIZE);
        return;
    }

    /* неизвестный magic — либо double free, либо чужой указатель.
       молча игнорируем, чтобы не рвать процесс на ровном месте. */
}
