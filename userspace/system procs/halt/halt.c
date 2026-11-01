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
    (void)argc;
    (void)argv;

    printf("Initiating system shutdown sequence...\n");

    // Вызываем системный вызов закрытия ОС
    int res = u_halt_os();

    // Если управление вернулось сюда — значит выключение сорвалось!
    // Анализируем возвращенный ядром код ошибки
    if (res == -1) { // HALT_ERR_OPEN_FILES
        printf("Halt failed: Target filesystem has active open files!\n");
        printf("Please close all active tasks and try again.\n");
    }
    else if (res == -2) { // HALT_ERR_VFS_DENIED
        printf("Halt failed: Low-level FS driver denied unmount protocol.\n");
    }
    else {
        printf("Halt failed: Unknown hardware or system error (code %d).\n", res);
    }

    // Легальный return 0 вернет фокус командной строке Сехмет
    return 0;
}
