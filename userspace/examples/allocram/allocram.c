#include "../../include/ustd.h"
#include "../../include/umemory.h"

int main(int argc, char *argv[]){
    printf("\n=== Userspace Dynamic RAM Allocator Test ===\n");

    //Тест 1: Мелкая аллокация (меньше размера страницы)
    printf("[TEST 1] Allocating 64 bytes for Buffer A... ");

    char *buf_a = (char*)malloc(64);

    if (buf_a != NULL) {
        printf("Success! Address: 0x%x\n", (uint32_t)buf_a);
        // Записываем тестовую строку
        buf_a[0] = 'O'; buf_a[1] = 'S'; buf_a[2] = '_'; buf_a[3] = 'O'; buf_a[4] = 'K'; buf_a[5] = '\0';
        printf("  Written content into Buffer A: %s\n", buf_a);
    } else {
        printf("FAILED!\n");
    }

    //Тест 2: Вторая аллокация (должна нарезаться из той же страницы памяти ядра)
    printf("[TEST 2] Allocating 128 bytes for Buffer B... ");
    char *buf_b = (char*)malloc(128);
    if (buf_b != NULL){
        printf("Success! Address: 0x%x\n", (uint32_t)buf_b);
        buf_b[0] = 'T'; buf_b[1] = 'E'; buf_b[2] = 'S'; buf_b[3] = 'T'; buf_b[4] = '\0';
        printf("  Written content into Buffer B: %s\n", buf_b);
    } else {
        printf("FAILED!\n");
    }

    //Тест 3: Проверка работы free и повторного использования памяти
    printf("[TEST 3] Freeing Buffer A and allocating Buffer C (64 bytes)... \n");
    free(buf_a); // Освобождаем Buffer A

    char *buf_c = (char*)malloc(64);
    printf("  Buffer C allocated at Address: 0x%x\n", (uint32_t)buf_c);
    if (buf_c == buf_a){
        printf("  Excellent! malloc() successfully reused the cleared slot of Buffer A!\n");
    } else {
        printf("  Note: Slot not reused, but address valid.\n");
    }

    //Зачищаем оставшиеся ресурсы перед выходом
    free(buf_b);
    free(buf_c);

    printf("=== Dynamic Memory Allocation Tests Completed Successfully ===\n");

    return 0;
}


