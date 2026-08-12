#include "include/ext2_driver.h"
#include "include/ext2_internal.h"
#include "../../include-kernel/bootinfo.h"


fs_driver_api_t api;

//  ----- внутренние функции  -----




void fs_init(struct boot_info *boot){

}

//  -----  внешние функции  -----


void driver_main(struct boot_info *boot){

    fs_init(boot);

    fs_driver_api_t *a = &api;

    //a->mount = fat12_mount;
}


fs_driver_api_t *driver_get_api(void){
    return &api;
}

