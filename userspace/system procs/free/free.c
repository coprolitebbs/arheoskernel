#include "../../include/ustd.h"
#include "../../include/umemory.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    am_mem_info_t mem;
    if (get_mem_info(&mem) != 0) {
        printf("Anubis: Cannot weigh system memory scales.\n");
        return -1;
    }

    //Переводим физические страницы (4096 байт) в Килобайты
    uint32_t total_kb = (mem.total_pages * 4096) / 1024;
    uint32_t used_kb  = (mem.used_pages * 4096) / 1024;
    uint32_t free_kb  = (mem.free_pages * 4096) / 1024;

    //Переводим байты кучи ядра в Килобайты
    uint32_t heap_total_kb = mem.heap_total_bytes / 1024;
    uint32_t heap_used_kb  = mem.heap_used_bytes / 1024;
    uint32_t heap_free_kb  = (mem.heap_total_bytes - mem.heap_used_bytes) / 1024;

    printf("\n=== Anubis Memory Scales (ARHeos Resources) ===\n");
    printf("Memory region     Total        Used         Free\n");
    printf("--------------------------------------------------\n");

    // Выводим физическое ОЗУ (PMM)
    printf("Physical RAM:    %d KB    %d KB    %d KB  (%d MB total)\n",
           total_kb, used_kb, free_kb, total_kb / 1024);

    // Выводим кучу ядра Ring 0 (kmalloc)
    printf("Kernel Heap:     %d KB    %d KB    %d KB\n",
           heap_total_kb, heap_used_kb, heap_free_kb);

    // Дополнительный расчет процента загрузки оперативной памяти
    uint32_t pct = (used_kb * 100) / total_kb;
    printf("--------------------------------------------------\n");
    printf("System RAM Load Indicator: %d%%\n\n", pct);

    return 0;
}

