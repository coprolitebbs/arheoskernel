 ; Copyright (C) 2026 ARHeOS by Dmitry H
 ;
 ; This program is free software: you can redistribute it and/or modify
 ; it under the terms of the GNU General Public License as published by
 ; the Free Software Foundation, either version 3 of the License, or
 ; (at your option) any later version.
 ;
 ; This program is distributed in the hope that it will be useful,
 ; but WITHOUT ANY WARRANTY; without even the implied warranty of
 ; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 ; GNU General Public License for more details.
 ;
 ; You should have received a copy of the GNU General Public License
 ; along with this program. If not, see <https://www.gnu.org/licenses/>.

;Общие структуры BootInfo

BOOTINFO_ADDR      equ 0x5000

; Адреса структур в памяти

BOOTINFO_ADDR      equ 0x5000

FSINFO_ADDR        equ 0x5100
MODULES_ADDR       equ 0x5200

BOOTINFO_VERSION   equ 2
BOOTINFO_MAGIC     equ 0x4F4F4249       ;"IBOO"

;Filesystem types

FS_UNKNOWN         equ 0
FS_FAT12           equ 1
FS_FAT16           equ 2
FS_EXT2            equ 3

;struct boot_info

BOOT_MAGIC         equ 0
BOOT_VERSION       equ 4
BOOT_SIZE          equ 8

BOOT_FRAMEBUFFER   equ 12
BOOT_PITCH         equ 16
BOOT_WIDTH         equ 20
BOOT_HEIGHT        equ 24
BOOT_BPP           equ 28
BOOT_GRAPHICS_FLAG equ 32

BOOT_E820_ADDR     equ 36
BOOT_E820_COUNT    equ 40

BOOT_FS_INFO       equ 44

BOOT_MODULE_COUNT  equ 48
BOOT_MODULE_TABLE  equ 52

BOOT_CMDLINE       equ 56
BOOT_LOADER_NAME   equ 60

BOOT_DRIVE         equ 64

;VBE: таблица режимов (заполняется vbe.inc до сборки bootinfo)
BOOT_VBE_MODES       equ 68
BOOT_VBE_MODE_COUNT  equ 72
BOOT_VBE_VERSION     equ 76
BOOT_VBE_CURRENT     equ 80

BOOT_TOTAL_SIZE      equ 84

;================= таблица режимов VBE =================
;Адрес и формат совпадают с include-kernel/bootinfo.h (vbe_table_hdr_t /
;vbe_mode_rec_t). Таблица лежит в младшей памяти и переживает старт ядра.

VBE_MODES_ADDR       equ 0x5400
VBE_MODES_MAX        equ 64
VBE_TABLE_MAGIC      equ 0x4D534256      ;"VBSM"

;шапка таблицы
VBE_TBL_MAGIC        equ 0
VBE_TBL_VERSION      equ 4
VBE_TBL_COUNT        equ 6
VBE_TBL_CURRENT      equ 8
VBE_TBL_RESERVED     equ 10
VBE_TBL_RECORDS      equ 12

;запись режима, 16 байт
VBE_REC_MODE         equ 0
VBE_REC_XRES         equ 2
VBE_REC_YRES         equ 4
VBE_REC_BPP          equ 6
VBE_REC_FLAGS        equ 7
VBE_REC_PITCH        equ 8
VBE_REC_RESERVED     equ 10
VBE_REC_PHYS         equ 12
VBE_MODE_REC_SIZE    equ 16
VBE_REC_FLAG_LFB     equ 1

;struct filesystem_info
;Универсальная часть

FS_TYPE                    equ 0

;общие параметры блока
FS_BLOCK_SIZE              equ 4

;FAT12/FAT16
FS_BYTES_PER_SECTOR        equ 8
FS_SECTORS_PER_CLUSTER     equ 12

;EXT2
FS_BLOCKS_PER_GROUP        equ 16
FS_INODES_PER_GROUP        equ 20
FS_INODE_SIZE              equ 24

;FAT
FS_RESERVED_SECTORS        equ 28
FS_FAT_COUNT               equ 32
FS_FAT_SIZE                equ 36
FS_ROOT_START              equ 40
FS_ROOT_SIZE               equ 44
FS_DATA_START              equ 48

; EXT2
FS_FIRST_DATA_BLOCK        equ 52
FSINFO_SIZE                equ 56

;module_info

MODULE_START        equ 0
MODULE_SIZE         equ 4
MODULE_TYPE         equ 8
MODULE_NAME         equ 12

MODULE_INFO_SIZE    equ 44

MODULE_TYPE_NONE           equ 0
MODULE_TYPE_DRIVER         equ 1
MODULE_TYPE_FILESYSTEM     equ 2
MODULE_KEYBOARD_DRIVER     equ 3
MODULE_BLOCKDEVICE_DRIVER  equ 4
MODULE_USB_DRIVER          equ 5

;MODULE_TYPE_SERVICE     equ 3
;MODULE_TYPE_PROGRAM     equ 4
