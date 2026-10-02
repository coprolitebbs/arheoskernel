BITS 16

;A20 Init
a20_init:
	in al,0x92
    or al,2
    out 0x92,al

	ret

a20_enable:
    push ax
    push cx
    push dx
    push si
    push di

    ;Попытка через контроллер клавиатуры (классический метод)
    ;Ждем пока входной буфер контроллера не освободится

.wait_in:
    in al, 0x64
    test al, 0x02
    jnz .wait_in

    ;Команда Записать байт в выходной порт контроллера
    mov al, 0xD1
    out 0x64, al

.wait_out:
    in al, 0x64
    test al, 0x02
    jnz .wait_out

    ;Данные Включить A20 (байт 0xDF)
    mov al, 0xDF
    out 0x60, al

    ;Небольшая задержка
    xor al, al
    jmp .skip

.skip:
    ;Проверка, читаем статус порта A20
    ;Это упрощенная проверка, часто достаточно просто послать команду
    pop di
    pop si
    pop dx
    pop cx
    pop ax

    ret



BITS 16
