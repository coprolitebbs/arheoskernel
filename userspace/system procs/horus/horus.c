//Гор - системный монитор

#include "../../include/ustd.h"


int main(int argc, char *argv[]){
    (void)argc;
    (void)argv;
    while(1) {
        //u_printf(400, 550, 0x00E0E0E0, "App live, ticks: %d", count++);
        yield();
    }

    return 0;
}
