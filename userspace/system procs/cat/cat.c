#include "../../include/ustd.h"
#include "../../../include-kernel/lib.h"

#define BUFFER_SIZE 254

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: cat [file_path]\n");
        return -1;
    }

    int fd = open(argv[1], FS_OPEN_READ);
    //printf("cat: fd = %d\n", fd);

    if (fd < 0) {
        printf("cat: %s: No such file or deity ritual\n", argv[1]);
        return -1;
    }

    char buffer[BUFFER_SIZE + 2];
    int bytes_read;

    while (1) {
        memset(buffer, 0, BUFFER_SIZE + 2);
        bytes_read = u_read(fd, buffer, BUFFER_SIZE);
        //printf("cat: br = %d\n", bytes_read);
        if (bytes_read <= 0) {
            break;
        }
        buffer[bytes_read] = '\0';
        printf("%s", buffer);
    }

    close(fd);
    printf("\n");
    return 0;
}
