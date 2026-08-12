BITS 32

; ---- Внешние функции из C ----
extern isr_handler
extern syscall_handler
extern idle_counter
global switch_task

global idle_loop



; ---- Макрос для обработчиков исключений без кода ошибки ----
%macro ISR_NOERR 1
    global isr%1
    isr%1:
        push byte 0          ; фиктивный код ошибки
        push byte %1         ; номер прерывания
        jmp isr_common
%endmacro

; ---- Макрос для обработчиков исключений с кодом ошибки ----
%macro ISR_ERR 1
    global isr%1
    isr%1:
        push byte %1         ; код ошибки уже в стеке
        jmp isr_common
%endmacro

; ---- Макрос для аппаратных прерываний (IRQ) ----
%macro IRQ 2
    global irq%1
    irq%1:
        push byte 0          ; фиктивный код ошибки
        push byte %2         ; номер вектора (32 + IRQ)
        jmp isr_common
%endmacro

; ---- Общий обработчик (сохраняет все регистры и вызывает C-функцию) ----
isr_common:
    pusha                 ; сохраняем все общие регистры
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10          ; селектор данных ядра
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp              ; передаём указатель на стек в C (struct regs*)
    call isr_handler
    add esp, 4            ; убираем аргумент

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8            ; убираем номер прерывания и код ошибки
    iret

; ---- Исключения (0–31) ----
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- Аппаратные прерывания (IRQ 0–15) ----
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; ---- Системный вызов (int 0x80) ----
global syscall_handler_asm

syscall_handler_asm:

    ; имитируем структуру regs

    push dword 0        ; err_code
    push dword 0x80     ; int_no


    pusha


    push ds
    push es
    push fs
    push gs


    mov ax,0x10
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax


    push esp
    call syscall_handler
    add esp,4


    pop gs
    pop fs
    pop es
    pop ds


    popa


    add esp,8


    iret
	
	


idle_loop:
    ;cli
.loop:
    inc dword [idle_counter]
	hlt
    jmp .loop
	
	
	
	
switch_task:
    mov eax,[esp+4]
    mov esp,eax

    pop gs
    pop fs
    pop es
    pop ds

    popa

    add esp,8
switch_ret:

    iret
	
	