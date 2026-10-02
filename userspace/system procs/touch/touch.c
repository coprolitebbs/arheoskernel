#include "../../include/ustd.h"
#include "../../../include-kernel/lib.h"

int main(int argc, char *argv[]){
    if (argc < 2) {
        printf("touch: missing file operand\n");
        printf("Usage: touch [file_path]\n");
        return -1;
    }
    for (int i = 1; i < argc; i++){
        const char *filename = argv[i];
        int fd = open(filename, FS_OPEN_CREATE);
        if (fd < 0){
            printf("touch: cannot touch '%s': file generation failed\n", filename);
            continue;
        }
        close(fd);
    }
    return 0;
}

