FAT12
 |
 |-- fat12_load_file()
 |
 +--> KERNEL
 |      0x20000
 |
 +--> FB_DR.DRV
        0x30000


build_fs_info()
        |
        v
filesystem_info_t


build_modules_info()
        |
        v
module_info_t[]
        |
        +-- start = 0x30000
        +-- size  = 0x2370
        +-- type  = DRIVER
        +-- name  = "FB_DR.DRV"


build_boot_info()
        |
        v
boot_info_t
        |
        +-- fs --------> filesystem_info_t
        |
        +-- modules --> module_info_t[]