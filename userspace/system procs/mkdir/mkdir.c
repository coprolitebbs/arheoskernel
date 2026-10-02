#include "../../include/ustd.h"
#include "../../../include-kernel/lib.h"

int main(int argc, char *argv[]){
    if (argc < 2) {
        printf("mkdir: missing operand\n");
        printf("Usage: mkdir [directory_name1] [directory_name2] ...\n");
        return -1;
    }
    for (int i = 1; i < argc; i++){
        const char *dirname = argv[i];
        int res = mkdir(dirname);
        if (res != 0) {
            printf("mkdir: cannot create directory '%s': access denied or already exists\n", dirname);
            continue;
        }    }
    return 0;
}

