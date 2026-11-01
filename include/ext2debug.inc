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

BITS 16

; dump_inode

; DS:SI -> inode

; EXT2 inode:
; +0  mode
; +2  uid
; +4  size low
; +40 block[0]

dump_inode:
    push ax
    push si
    mov ax,[si+0]
    call debug_hex_word
    mov ax,[si+2]
    call debug_hex_word
    mov ax,[si+4]
    call debug_hex_word
    mov ax,[si+40]
    call debug_hex_word
    call newline
    pop si
    pop ax

    ret


; dump_inode_bytes

; DS:SI -> inode

; dump first 64 bytes

dump_inode_bytes:
    push ax
    push bx
    push cx
    push si
    xor bx,bx
    mov cx,32

.loop:
    mov ax,[si+bx]
    push bx
    push cx
    push si
    call debug_hex_word
    pop si
    pop cx
    pop bx
    add bx,2
    loop .loop
    call newline
    pop si
    pop cx
    pop bx
    pop ax

    ret


;Дамп первых 32 байт ядра
dump_kernel:
    pushad
    push ds
    push es
    push fs

    ; --- ВЫВОД ЗНАЧЕНИЯ KERNEL_SIZE_PM В ПОРТ 0xE9 ---
    ; Настраиваем DS на сегмент кода Stage 2 (CS), чтобы прочитать переменную
    mov ax, cs
    mov ds, ax

    ; Печатаем маркер размера [S2_SIZE]:
    mov al, '['
    call debug_char
    mov al, 'S'
    call debug_char
    mov al, '2'
    call debug_char
    mov al, '_'
    call debug_char
    mov al, 'S'
    call debug_char
    mov al, 'I'
    call debug_char
    mov al, 'Z'
    call debug_char
    mov al, 'E'
    call debug_char
    mov al, ']'
    call debug_char
    mov al, ' '
    call debug_char

    ; Считываем 32-битное значение размера файла ядра
    mov ebx, [cs:kernel_size_pm]

    ; Выводим ebx в HEX-виде в порт 0xE9 (8 нибблов)
    mov ecx, 8
.size_hex_loop:
    rol ebx, 4
    mov al, bl
    and al, 0x0F
    cmp al, 10
    jb .sz_digit
    add al, 'A' - 10
    jmp .sz_out
.sz_digit:
    add al, '0'
.sz_out:
    call debug_char
    loop .size_hex_loop

    ; Перевод строки после вывода размера
    mov al, 13
    call debug_char
    mov al, 10
    call debug_char

    ; --- ДАЛЕЕ ИДЕТ СТАНДАРТНЫЙ ВХОД В UNREAL MODE И ДАМП ПАМЯТИ (без изменений) ---
    cli
    lgdt [cs:gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp .pm_flush
.pm_flush:
    mov ax, 0x10
    mov fs, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp .rm_flush
.rm_flush:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    sti

    mov esi, 0x100000
    mov cx, 64

.next_byte:
    mov al, [fs:esi]
    mov dl, al
    shr al, 4
    call .nibble_to_hex
    call .output_char
    mov al, dl
    and al, 0x0F
    call .nibble_to_hex
    call .output_char
    mov al, ' '
    call .output_char
    inc esi
    mov eax, esi
    sub eax, 0x100000
    and eax, 15
    jnz .loop_end
    mov al, 13
    call .output_char
    mov al, 10
    call .output_char
.loop_end:
    dec cx
    jnz .next_byte

    pop fs
    pop es
    pop ds
    popad
    ret

.nibble_to_hex:
    cmp al, 10
    jb .is_digit
    add al, 'A' - 10
    ret
.is_digit:
    add al, '0'
    ret

.output_char:
    call debug_char
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop bx
    pop ax
    ret



dump_driver:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    mov ax,0x5000
    mov ds,ax
    xor si,si
    mov cx,128

.next:
    lodsb
    ;старшая тетрада
    mov ah,al
    shr ah,4
    mov al,ah
    call .print_nibble
    ;младшая
    mov al,[si-1]
    and al,0Fh
    call .print_nibble
    mov al,' '
    mov ah,0Eh
    int 10h
    ;каждые 16 байт новая строка
    test si,15
    jnz .cont
    mov al,13
    mov ah,0Eh
    int 10h
    mov al,10
    mov ah,0Eh
    int 10h

.cont:
    loop .next
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    ret

.print_nibble:
    cmp al,10
    jb .digit
    add al,'A'-10
    jmp .out

.digit:
    add al,'0'

.out:
    mov ah,0Eh
    int 10h

    ret



; Messages
