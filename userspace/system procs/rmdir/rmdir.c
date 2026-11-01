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
#include "../../../include-kernel/lib.h"

int main(int argc, char *argv[]){
    if (argc < 2) {
        printf("rmdir: missing operand\n");
        printf("Usage: rmdir [directory_name1] ...\n");
        return -1;
    }
    for (int i = 1; i < argc; i++){
        const char *dirname = argv[i];
        int res = rmdir(dirname);
        if (res != 0) {
            printf("rmdir: cannot remove '%s': directory not empty or no such folder\n", dirname);
            continue;
        }
    }
    return 0;
}
