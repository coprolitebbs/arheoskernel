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

int main(int argc, char *argv[]) {
    clear_screen();

    printf("=== ARheOS Userspace scanf Test ===\n");
    printf("Введите ваш возраст (число) и кодовое имя (строку):\n");

    int age = 0;
    char name[64];

    // u_read() внутри ustd.c заблокирует поток, дождется ввода строки лога,
    // а наш новый scanf распарсит её по переменным!
    int scanned = scanf("%d %s", &age, name);

    printf("\nРезолюция scanf (успешных совпадений: %d):\n", scanned);
    printf("  Ваш возраст: %d лет\n", age);
    printf("  Ваше имя:    \"%s\"\n", name);

    return 0;
}

