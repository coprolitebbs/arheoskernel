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

#define BUFFER_SIZE 254

int main(int argc, char *argv[]){
    int fd;
    bool close_after = false;

    if (argc < 2){
        fd = STDIN;
    } else {
        fd = open(argv[1], FS_OPEN_READ);
        if (fd < 0){
            printf("cat: %s: No such file or deity ritual\n", argv[1]);
            return -1;
        }
        close_after = true;
    }

    char buffer[BUFFER_SIZE + 2];
    int bytes_read;
    while (1){
        memset(buffer, 0, BUFFER_SIZE + 2);
        bytes_read = u_read(fd, buffer, BUFFER_SIZE);
        if (bytes_read <= 0) break;
        buffer[bytes_read] = '\0';
        printf("%s", buffer);
    }

    if (close_after) close(fd);
    printf("\n");
    return 0;
}
