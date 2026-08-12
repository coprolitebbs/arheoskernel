; ============================================================
; bootinfo.inc
; Общие структуры BootInfo
; ============================================================

BOOTINFO_ADDR      equ 0x5000

; ============================================================
; Адреса структур в памяти
; ============================================================

BOOTINFO_ADDR      equ 0x5000

FSINFO_ADDR        equ 0x5100
MODULES_ADDR       equ 0x5200

BOOTINFO_VERSION   equ 1
BOOTINFO_MAGIC     equ 0x4F4F4249       ; "IBOO"


; ============================================================
; Filesystem types
; ============================================================

FS_UNKNOWN         equ 0
FS_FAT12           equ 1
FS_FAT16           equ 2
FS_EXT2            equ 3



; ============================================================
; struct boot_info
;
; ============================================================

BOOT_MAGIC         equ 0
BOOT_VERSION       equ 4
BOOT_SIZE          equ 8


BOOT_FRAMEBUFFER   equ 12
BOOT_PITCH         equ 16
BOOT_WIDTH         equ 20
BOOT_HEIGHT        equ 24
BOOT_BPP           equ 28


BOOT_E820_ADDR     equ 32
BOOT_E820_COUNT    equ 36


BOOT_FS_INFO       equ 40


BOOT_MODULE_COUNT  equ 44
BOOT_MODULE_TABLE  equ 48


BOOT_CMDLINE       equ 52
BOOT_LOADER_NAME   equ 56

BOOT_DRIVE         equ 60

BOOT_TOTAL_SIZE    equ 64



; ============================================================
; struct filesystem_info
;
; Универсальная часть
;
; ============================================================

FS_TYPE                    equ 0


; общие параметры блока
FS_BLOCK_SIZE              equ 4


; FAT12/FAT16
FS_BYTES_PER_SECTOR        equ 8
FS_SECTORS_PER_CLUSTER     equ 12


; EXT2
FS_BLOCKS_PER_GROUP        equ 16
FS_INODES_PER_GROUP        equ 20
FS_INODE_SIZE              equ 24


; FAT
FS_RESERVED_SECTORS        equ 28
FS_FAT_COUNT               equ 32
FS_FAT_SIZE                equ 36
FS_ROOT_START              equ 40
FS_ROOT_SIZE               equ 44
FS_DATA_START              equ 48


; EXT2
FS_FIRST_DATA_BLOCK        equ 52
FSINFO_SIZE                equ 56



; ============================================================
; module_info
; ============================================================

MODULE_START        equ 0
MODULE_SIZE         equ 4
MODULE_TYPE         equ 8
MODULE_NAME         equ 12

MODULE_INFO_SIZE    equ 44


MODULE_TYPE_NONE        equ 0
MODULE_TYPE_DRIVER      equ 1
MODULE_TYPE_FILESYSTEM  equ 2
MODULE_TYPE_SERVICE     equ 3
MODULE_TYPE_PROGRAM     equ 4
