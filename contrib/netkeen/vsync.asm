bits 16
org 0
; Replacement for NetKeen 2013-03-26 VW_SetScreen / VL_WaitDisplay.
; Keep the entry at +81h and its stack frame, so all callers still work.
set_screen:
    pushf
    sti
    mov dx, 03dah
wait_retrace_end:
    in al, dx
    test al, 8
    jnz wait_retrace_end
    ; Both pending registers now belong to the *next* retrace.
    cli
    mov cx, [bp+6]
    mov dx, 03d4h
    mov al, 0ch
    out dx, al
    inc dx
    mov al, ch
    out dx, al
    dec dx
    mov al, 0dh
    out dx, al
    inc dx
    mov al, cl
    out dx, al
    test word [0b308h], 1 ; NetKeen's no_pan flag
    jnz no_panning
    mov dx, 03c0h
    mov al, 033h
    out dx, al
    mov al, [bp+8]
    out dx, al
no_panning:
    sti
    mov dx, 03dah
wait_retrace_start:
    in al, dx
    test al, 8
    jz wait_retrace_start
    ; The new display address is latched. The game may reuse its old page.
    popf
    leave
    retf
    times 081h-($-$$) db 090h
entry:
    enter 0, 0
    jmp near set_screen
    times 0b5h-($-$$) db 090h
