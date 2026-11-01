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

;FAT12 filesystem info builder

build_fs_info:
    push edi
    mov edi,FSINFO_ADDR

    mov dword [edi+FS_TYPE],FS_FAT12

    mov eax,0
    mov ax,[bps]
    mov [edi+FS_BYTES_PER_SECTOR],eax

    mov eax,0
    mov ax,[spc]
    mov [edi+FS_SECTORS_PER_CLUSTER],eax

    mov eax,0
    mov ax,[rsvd]
    mov [edi+FS_RESERVED_SECTORS],eax

    mov eax,0
    mov ax,[nfats]
    mov [edi+FS_FAT_COUNT],eax

    mov eax,0
    mov ax,[fatsz]
    mov [edi+FS_FAT_SIZE],eax

    mov eax,0
    mov ax,[root_start]
    mov [edi+FS_ROOT_START],eax

    mov eax,0
    mov ax,[root_size]
    mov [edi+FS_ROOT_SIZE],eax

    mov eax,0
    mov ax,[data_start]
    mov [edi+FS_DATA_START],eax

    pop edi

    ret
