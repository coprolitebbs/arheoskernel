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

; EXT2 filesystem info builder

build_fs_info:
    push edi
    mov edi,FSINFO_ADDR
    mov dword [edi+FS_TYPE],FS_EXT2

    xor eax,eax
    mov ax,[ext2_block_size]
    mov [edi+FS_BLOCK_SIZE],eax

    ;blocks per group

    xor eax,eax
    mov ax,[ext2_blocks_per_group]
    mov [edi+FS_BLOCKS_PER_GROUP],eax

    ;inodes per group

    xor eax,eax
    mov ax,[ext2_inodes_per_group]
    mov [edi+FS_INODES_PER_GROUP],eax

    ;inode size

    xor eax,eax
    mov ax,[ext2_inode_size]
    mov [edi+FS_INODE_SIZE],eax

    ;first data block

    xor eax,eax
    mov ax,[ext2_first_data_block]
    mov [edi+FS_FIRST_DATA_BLOCK],eax

    pop edi

    ret
