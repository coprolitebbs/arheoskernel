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
