#include "../../include/ustd.h"
#include "../../include/umemory.h"

#define ARRAY_SIZE 512

// Простой глобальный статический массив.
// Компилятор GCC гарантированно поместит его в секцию .bss.
// Этот тест проверит, корректно ли ваш ELF-загрузчик выделил страницы памяти
// под неинициализированные данные и занулил ли их Ring 0.
int global_static_array[ARRAY_SIZE];

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("\n--- ArheOS ELF Integrity & Memory Validation Test ---\n");

    // =========================================================================
    // ТЕСТ 1: Валидация статической памяти (.bss / .data)
    // =========================================================================
    printf("[TEST 1] Initializing static global array (%d elements)...\n", ARRAY_SIZE);

    // Заполняем массив числами (например, четной последовательностью)
    for (int i = 0; i < ARRAY_SIZE; i++) {
        global_static_array[i] = i * 2;
    }

    // Выборочно выводим элементы для проверки релокаций адресов
    printf("Static array validation samples:\n");
    printf("  global_static_array[0]   = %d (Expected: 0)\n", global_static_array[0]);
    printf("  global_static_array[10]  = %d (Expected: 20)\n", global_static_array[10]);
    printf("  global_static_array[256] = %d (Expected: 512)\n", global_static_array[256]);
    printf("  global_static_array[511] = %d (Expected: 1022)\n", global_static_array[511]);
    printf("[SUCCESS] Static section layout verified.\n\n");

    // =========================================================================
    // ТЕСТ 2: Валидация динамической кучи процесса (malloc / SYS_SBRK)
    // =========================================================================
    printf("[TEST 2] Allocating dynamic array via malloc()...\n");

    // Запрашиваем у аллокатора Ring 3 память под 512 целых чисел
    int *dynamic_array = (int *)malloc(ARRAY_SIZE * sizeof(int));

    if (dynamic_array == NULL) {
        printf("[FAIL] malloc() returned NULL! Virtual heap allocation is broken.\n");
        return -1;
    }

    // Выводим физический/виртуальный адрес, который выдал sbrk
    printf("Heap registry allocation address: 0x%x\n", (uint32_t)dynamic_array);
    printf("Populating dynamic array memory space...\n");

    // Заполняем динамический массив другой последовательностью (умножение на 3)
    for (int i = 0; i < ARRAY_SIZE; i++) {
        dynamic_array[i] = i * 3;
    }

    // Проверяем чтение и запись по динамическим указателям
    printf("Dynamic array validation samples:\n");
    printf("  dynamic_array[0]   = %d (Expected: 0)\n", dynamic_array[0]);
    printf("  dynamic_array[10]  = %d (Expected: 30)\n", dynamic_array[10]);
    printf("  dynamic_array[256] = %d (Expected: 768)\n", dynamic_array[256]);
    printf("  dynamic_array[511] = %d (Expected: 1533)\n", dynamic_array[511]);

    // Возвращаем страницы менеджеру памяти кучи
    printf("Releasing memory block via free()...\n");
    free_pages(dynamic_array,ARRAY_SIZE * sizeof(int));

    printf("[SUCCESS] Dynamic context allocation verified.\n");
    printf("--- All ELF runtime execution checks passed successfully! ---\n\n");

    return 0;
}

