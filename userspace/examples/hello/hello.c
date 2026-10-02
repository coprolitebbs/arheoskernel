#include "../../include/ustd.h"

int main(int argc, char *argv[]){
    printf("\n=== Userspace CLI Arguments Parsing Test ===\n");
    printf("Total arguments count (argc): %d\n", argc);

    for (int i = 0; i < argc; i++){
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    }

    printf("=== CLI Parsing Test Completed Successfully ===\n");
    return 0;
}

