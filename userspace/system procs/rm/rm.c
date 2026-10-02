#include "../../include/ustd.h"
#include "../../../include-kernel/lib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("rm: missing operand\n");
        printf("Usage: rm [file_path1] [file_path2] ...\n");
        return -1;
    }
    for (int i = 1; i < argc; i++) {
        const char *filename = argv[i];
        int res = unlink(filename);
        if (res != 0) {
            printf("rm: cannot remove '%s': no such file or access denied, code %d\n", filename,res);
            continue;
        }
    }
    return 0;
}

