/*
 * Copyright (C) 2026 ARHeOS by Dmitry H
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "../../include/ustd.h"
#include "../../include/umemory.h"

int main(int argc, char *argv[]){
    printf("=== Userspace Dynamic RAM Allocator Test ===\n");

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


