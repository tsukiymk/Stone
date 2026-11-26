; x64 scalar JNI bridge. A small per-target thunk places a descriptor or a
; Windows function address in r10 before tail-jumping to these entry points.
OPTION CASEMAP:NONE
EXTERN StoneGuestEnv:PROC
EXTERN StoneGuestThreadPointer:PROC
EXTERN StoneCurrentGuestThreadPointer:PROC
EXTERN StoneInvokeAndroidScalars:PROC
EXTERN StoneEnterAndroidNamespace:PROC
EXTERN StoneLeaveAndroidNamespace:PROC
EXTERN StoneAndroidPrint:PROC
EXTERN StoneAndroidStdout:PROC
EXTERN StoneAndroidPrintDescriptor:PROC
EXTERN StoneAndroidLog:PROC
EXTERN StoneAndroidLogAssert:PROC
EXTERN StoneAndroidScan:PROC
EXTERN StoneAndroidFormat:PROC
EXTERN StoneAndroidUnboundedFormat:PROC
EXTERN StoneAndroidWideFormat:PROC
EXTERN StoneAndroidParseLongDouble:PROC
EXTERN StoneAndroidWideParseLongDouble:PROC
EXTERN StoneAndroidAllocateFormat:PROC
EXTERN StoneRunArm64Native:PROC
EXTERN StoneGetAndroidSignalMask:PROC
EXTERN StoneRestoreAndroidSignalMask:PROC
EXTERN __chkstk:PROC
PUBLIC WindowsToAndroid, CallAndroid, AndroidToWindows, AndroidToWindowsFloatFirst
PUBLIC AndroidToWindowsScalars
PUBLIC CallAndroidFive
PUBLIC AndroidToWindowsWide, AndroidToWindowsIntFirst, AndroidVarargsPrint, AndroidVarargsLog
PUBLIC AndroidToWindowsWideEleven
PUBLIC AndroidToWindowsCodecCallbacks
PUBLIC AndroidVarargsStdout, AndroidVarargsDescriptor
PUBLIC AndroidJniVarargs
PUBLIC AndroidVarargsScan, AndroidVarargsFormat
PUBLIC AndroidVarargsUnboundedFormat
PUBLIC AndroidVarargsWideFormat
PUBLIC AndroidParseLongDouble, AndroidWideParseLongDouble
PUBLIC AndroidVarargsAllocateFormat
PUBLIC AndroidVarargsLogAssert
PUBLIC AndroidSetJmp, AndroidSignalSetJmp, AndroidLongJmp
PUBLIC AndroidLongLongDiv
PUBLIC Arm64NativeEntry

.code

; r10 carries the translated method descriptor. Save Win64 scalar argument
; slots before C++ repacks them for AAPCS64. Return bits in both result classes.
Arm64NativeEntry PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    movq qword ptr [rsp+40h], xmm0
    movq qword ptr [rsp+48h], xmm1
    movq qword ptr [rsp+50h], xmm2
    movq qword ptr [rsp+58h], xmm3
    lea rax, [rsp+90h]
    mov [rsp+60h], rax
    mov rcx, r10
    lea rdx, [rsp+20h]
    call StoneRunArm64Native
    movq xmm0, rax
    add rsp, 68h
    ret
Arm64NativeEntry ENDP

; The saved frame and the jump target both use the guest SysV ABI.
AndroidSetJmp PROC
    mov esi, 1
    jmp AndroidSignalSetJmp
AndroidSetJmp ENDP

AndroidSignalSetJmp PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    mov [rdi], rbx
    mov [rdi+8], rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    lea rax, [rsp+30h]
    mov [rdi+48], rax
    mov rax, [rsp+28h]
    mov [rdi+56], rax
    mov qword ptr [rdi+64], 0
    test esi, esi
    jz setjmp_done
    call StoneGetAndroidSignalMask
    mov [rdi+72], rax
    mov qword ptr [rdi+64], 1
setjmp_done:
    xor eax, eax
    add rsp, 28h
    ret
AndroidSignalSetJmp ENDP

AndroidLongJmp PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    test byte ptr [rdi+64], 1
    jz longjmp_restore
    mov rcx, [rdi+72]
    call StoneRestoreAndroidSignalMask
longjmp_restore:
    mov eax, esi
    test eax, eax
    jne longjmp_value
    inc eax
longjmp_value:
    mov rbx, [rdi]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]
    jmp qword ptr [rdi+56]
AndroidLongJmp ENDP

; Android x86_64 returns the quotient and remainder in RAX and RDX.
AndroidLongLongDiv PROC
    mov rax, rdi
    cqo
    idiv rsi
    ret
AndroidLongLongDiv ENDP

; CallAndroid(descriptor, arg0, ...) is used for constructors and host callbacks.
CallAndroid PROC
    mov r10, rcx
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    mov r9, [rsp+28h]
    jmp WindowsToAndroid
CallAndroid ENDP

CallAndroidFive PROC
    mov rax, [rsp+30h]
    mov r10, rcx
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    mov r9, [rsp+28h]
    mov [rsp+28h], rax
    jmp WindowsToAndroid
CallAndroidFive ENDP

WindowsToAndroid PROC FRAME
    push rdi
    .pushreg rdi
    push rsi
    .pushreg rsi
    sub rsp, 288h
    .allocstack 288h
    movdqa [rsp+1d0h], xmm6
    .savexmm128 xmm6, 1d0h
    movdqa [rsp+1e0h], xmm7
    .savexmm128 xmm7, 1e0h
    movdqa [rsp+1f0h], xmm8
    .savexmm128 xmm8, 1f0h
    movdqa [rsp+200h], xmm9
    .savexmm128 xmm9, 200h
    movdqa [rsp+210h], xmm10
    .savexmm128 xmm10, 210h
    movdqa [rsp+220h], xmm11
    .savexmm128 xmm11, 220h
    movdqa [rsp+230h], xmm12
    .savexmm128 xmm12, 230h
    movdqa [rsp+240h], xmm13
    .savexmm128 xmm13, 240h
    movdqa [rsp+250h], xmm14
    .savexmm128 xmm14, 250h
    movdqa [rsp+260h], xmm15
    .savexmm128 xmm15, 260h
    .endprolog

    ; 0x00..0xc7 is outgoing stack space, 0xd0..0x1cf holds incoming arguments.
    mov [rsp+270h], r10
    mov [rsp+0d0h], rcx
    mov [rsp+0d8h], rdx
    mov [rsp+0e0h], r8
    mov [rsp+0e8h], r9
    mov qword ptr [rsp+1c8h], 0
    mov edx, [r10+12]
    test edx, 1
    jz arg1
    movsd qword ptr [rsp+0d0h], xmm0
arg1:
    test edx, 2
    jz arg2
    movsd qword ptr [rsp+0d8h], xmm1
arg2:
    test edx, 4
    jz arg3
    movsd qword ptr [rsp+0e0h], xmm2
arg3:
    test edx, 8
    jz stack_args
    movsd qword ptr [rsp+0e8h], xmm3
stack_args:
    mov eax, 4
copy_arg:
    cmp eax, [r10+8]
    jae adapt_env
    mov rcx, [rsp+2a0h+rax*8]
    mov [rsp+0d0h+rax*8], rcx
    inc eax
    jmp copy_arg
adapt_env:
    cmp byte ptr [r10+30], 0
    je load_args
    mov rcx, [rsp+0d0h]
    call StoneGuestEnv
    mov [rsp+0d0h], rax
    mov r10, [rsp+270h]
load_args:
    mov rcx, [r10+64]
    test rcx, rcx
    jz prepare_guest_stack
    call StoneEnterAndroidNamespace
    mov [rsp+280h], rax
    mov r10, [rsp+270h]
prepare_guest_stack:
    cmp byte ptr [r10+31], 0
    je load_registers
    ; ELF functions may allocate a large frame without probing each Windows
    ; guard page. Commit space before entering the guest.
    mov eax, 40000h
    call __chkstk
    mov r10, [rsp+270h]
    ; Restore the Windows thread's previous FS base when leaving the guest.
    rdfsbase rax
    mov [rsp+278h], rax
    call StoneGuestThreadPointer
    wrfsbase rax
    mov r10, [rsp+270h]
load_registers:
    xor r11d, r11d
copy_stack:
    cmp r11d, [r10+60]
    jae register_args
    movzx eax, byte ptr [r10+32+r11]
    mov rcx, [rsp+0d0h+rax*8]
    mov [rsp+r11*8], rcx
    inc r11d
    jmp copy_stack
register_args:
    movzx eax, byte ptr [r10+16]
    mov rdi, [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+17]
    mov rsi, [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+18]
    mov rdx, [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+19]
    mov rcx, [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+20]
    mov r8, [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+21]
    mov r9, [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+22]
    movsd xmm0, qword ptr [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+23]
    movsd xmm1, qword ptr [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+24]
    movsd xmm2, qword ptr [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+25]
    movsd xmm3, qword ptr [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+26]
    movsd xmm4, qword ptr [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+27]
    movsd xmm5, qword ptr [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+28]
    movsd xmm6, qword ptr [rsp+0d0h+rax*8]
    movzx eax, byte ptr [r10+29]
    movsd xmm7, qword ptr [rsp+0d0h+rax*8]
    call qword ptr [r10]

    mov r10, [rsp+270h]
    cmp byte ptr [r10+31], 0
    je restore_registers
    mov r11, [rsp+278h]
    wrfsbase r11
restore_registers:
    cmp qword ptr [r10+64], 0
    je restore_nonvolatile
    mov [rsp+0d0h], rax
    mov [rsp+0d8h], rdx
    movups [rsp+0e0h], xmm0
    movups [rsp+0f0h], xmm1
    mov rcx, [rsp+280h]
    call StoneLeaveAndroidNamespace
    mov rax, [rsp+0d0h]
    mov rdx, [rsp+0d8h]
    movups xmm0, [rsp+0e0h]
    movups xmm1, [rsp+0f0h]
restore_nonvolatile:

    movdqa xmm6, [rsp+1d0h]
    movdqa xmm7, [rsp+1e0h]
    movdqa xmm8, [rsp+1f0h]
    movdqa xmm9, [rsp+200h]
    movdqa xmm10, [rsp+210h]
    movdqa xmm11, [rsp+220h]
    movdqa xmm12, [rsp+230h]
    movdqa xmm13, [rsp+240h]
    movdqa xmm14, [rsp+250h]
    movdqa xmm15, [rsp+260h]
    add rsp, 288h
    pop rsi
    pop rdi
    ret
WindowsToAndroid ENDP

; Windows may clear FS while returning from a system call. Preserve the guest
; thread pointer across imports and keep the original two-register return value.
RESTORE_GUEST_FS MACRO scratch
    LOCAL done
    mov [rsp+scratch], rax
    mov [rsp+scratch+8], rdx
    movups [rsp+scratch+10h], xmm0
    movups [rsp+scratch+20h], xmm1
    call StoneCurrentGuestThreadPointer
    test rax, rax
    jz done
    wrfsbase rax
done:
    mov rax, [rsp+scratch]
    mov rdx, [rsp+scratch+8]
    movups xmm0, [rsp+scratch+10h]
    movups xmm1, [rsp+scratch+20h]
ENDM

; Fixed integer/pointer or all-floating-point arguments from the Android image.
AndroidToWindowsScalars PROC FRAME
    sub rsp, 0d8h
    .allocstack 0d8h
    .endprolog
    mov [rsp+20h], rdi
    mov [rsp+28h], rsi
    mov [rsp+30h], rdx
    mov [rsp+38h], rcx
    mov [rsp+40h], r8
    mov [rsp+48h], r9
    movq qword ptr [rsp+50h], xmm0
    movq qword ptr [rsp+58h], xmm1
    movq qword ptr [rsp+60h], xmm2
    movq qword ptr [rsp+68h], xmm3
    movq qword ptr [rsp+70h], xmm4
    movq qword ptr [rsp+78h], xmm5
    movq qword ptr [rsp+80h], xmm6
    movq qword ptr [rsp+88h], xmm7
    lea rax, [rsp+0e0h]
    mov [rsp+90h], rax
    mov rcx, r10
    lea rdx, [rsp+20h]
    call StoneInvokeAndroidScalars
    movq xmm0, rax
    RESTORE_GUEST_FS 0a0h
    add rsp, 0d8h
    ret
AndroidToWindowsScalars ENDP

AndroidToWindows PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    mov [rsp+20h], r8
    mov [rsp+28h], r9
    mov r9, rcx
    mov r8, rdx
    mov rdx, rsi
    mov rcx, rdi
    call r10
    RESTORE_GUEST_FS 30h
    add rsp, 68h
    ret
AndroidToWindows ENDP

; SysV puts the four-pointer callback structure on the stack, while both
; scalar arguments still occupy registers. Win64 passes the structure by reference.
AndroidToWindowsCodecCallbacks PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    mov rcx, rdi
    lea rdx, [rsp+70h]
    mov r8, rsi
    call r10
    RESTORE_GUEST_FS 30h
    add rsp, 68h
    ret
AndroidToWindowsCodecCallbacks ENDP

; The SysV ABI returns extended-precision values in st(0). The Windows
; helper supplies the ten bytes of the x87 representation in a local slot.
ANDROID_LONG_DOUBLE MACRO entry, target
entry PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    mov r8, rsi
    mov rdx, rdi
    lea rcx, [rsp+20h]
    call target
    RESTORE_GUEST_FS 30h
    fld tbyte ptr [rsp+20h]
    add rsp, 68h
    ret
entry ENDP
ENDM

ANDROID_LONG_DOUBLE AndroidParseLongDouble, StoneAndroidParseLongDouble
ANDROID_LONG_DOUBLE AndroidWideParseLongDouble, StoneAndroidWideParseLongDouble

; A float in xmm0 followed by up to three integer/pointer arguments.
AndroidToWindowsFloatFirst PROC FRAME
    sub rsp, 58h
    .allocstack 58h
    .endprolog
    mov r9, rdx
    mov r8, rsi
    mov rdx, rdi
    call r10
    RESTORE_GUEST_FS 20h
    add rsp, 58h
    ret
AndroidToWindowsFloatFirst ENDP

; One integer followed by up to four floats, as used by glUniform*f.
AndroidToWindowsIntFirst PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    movss dword ptr [rsp+20h], xmm3
    movaps xmm3, xmm2
    movaps xmm2, xmm1
    movaps xmm1, xmm0
    mov rcx, rdi
    call r10
    RESTORE_GUEST_FS 30h
    add rsp, 68h
    ret
AndroidToWindowsIntFirst ENDP

; Integer and pointer arguments for GLES calls with up to ten or eleven parameters.
ANDROID_WIDE MACRO entry, count
entry PROC FRAME
    sub rsp, 88h
    .allocstack 88h
    .endprolog
    mov [rsp+20h], r8
    mov [rsp+28h], r9
    mov rax, [rsp+90h]
    mov [rsp+30h], rax
    mov rax, [rsp+98h]
    mov [rsp+38h], rax
    mov rax, [rsp+0a0h]
    mov [rsp+40h], rax
    mov rax, [rsp+0a8h]
    mov [rsp+48h], rax
    IF count EQ 11
        mov rax, [rsp+0b0h]
        mov [rsp+50h], rax
    ENDIF
    mov r9, rcx
    mov r8, rdx
    mov rdx, rsi
    mov rcx, rdi
    call r10
    RESTORE_GUEST_FS 58h
    add rsp, 88h
    ret
entry ENDP
ENDM
ANDROID_WIDE AndroidToWindowsWide, 10
ANDROID_WIDE AndroidToWindowsWideEleven, 11

; Save the SysV argument registers for the host's va_list adapter.
SAVE_VARARGS MACRO
    mov [rsp+40h], rdi
    mov [rsp+48h], rsi
    mov [rsp+50h], rdx
    mov [rsp+58h], rcx
    mov [rsp+60h], r8
    mov [rsp+68h], r9
    movaps [rsp+70h], xmm0
    movaps [rsp+80h], xmm1
    movaps [rsp+90h], xmm2
    movaps [rsp+0a0h], xmm3
    movaps [rsp+0b0h], xmm4
    movaps [rsp+0c0h], xmm5
    movaps [rsp+0d0h], xmm6
    movaps [rsp+0e0h], xmm7
    mov dword ptr [rsp+20h+4], 48
    lea rax, [rsp+100h]
    mov [rsp+28h], rax
    lea rax, [rsp+40h]
    mov [rsp+30h], rax
ENDM

ANDROID_PRINT_VARARGS MACRO entry, target, fixed
entry PROC FRAME
    sub rsp, 0f8h
    .allocstack 0f8h
    .endprolog
    SAVE_VARARGS
    mov dword ptr [rsp+20h], fixed * 8
    mov rcx, rdi
    IF fixed EQ 1
        lea rdx, [rsp+20h]
    ELSE
        mov rdx, rsi
        lea r8, [rsp+20h]
    ENDIF
    call target
    RESTORE_GUEST_FS 40h
    add rsp, 0f8h
    ret
entry ENDP
ENDM
ANDROID_PRINT_VARARGS AndroidVarargsPrint, StoneAndroidPrint, 2
ANDROID_PRINT_VARARGS AndroidVarargsStdout, StoneAndroidStdout, 1
ANDROID_PRINT_VARARGS AndroidVarargsDescriptor, StoneAndroidPrintDescriptor, 2
ANDROID_PRINT_VARARGS AndroidVarargsUnboundedFormat, StoneAndroidUnboundedFormat, 2

ANDROID_LOG_VARARGS MACRO entry, target
entry PROC FRAME
    sub rsp, 0f8h
    .allocstack 0f8h
    .endprolog
    SAVE_VARARGS
    mov dword ptr [rsp+20h], 24
    mov rcx, rdi
    mov r8, rdx
    mov rdx, rsi
    lea r9, [rsp+20h]
    call target
    RESTORE_GUEST_FS 40h
    add rsp, 0f8h
    ret
entry ENDP
ENDM

ANDROID_LOG_VARARGS AndroidVarargsLog, StoneAndroidLog
ANDROID_LOG_VARARGS AndroidVarargsLogAssert, StoneAndroidLogAssert

AndroidVarargsScan PROC FRAME
    sub rsp, 0f8h
    .allocstack 0f8h
    .endprolog
    SAVE_VARARGS
    mov dword ptr [rsp+20h], 16
    mov rcx, rdi
    mov rdx, rsi
    lea r8, [rsp+20h]
    call StoneAndroidScan
    RESTORE_GUEST_FS 40h
    add rsp, 0f8h
    ret
AndroidVarargsScan ENDP

ANDROID_BUFFER_VARARGS MACRO entry, target
entry PROC FRAME
    sub rsp, 0f8h
    .allocstack 0f8h
    .endprolog
    SAVE_VARARGS
    mov dword ptr [rsp+20h], 24
    mov rcx, rdi
    mov r8, rdx
    mov rdx, rsi
    lea r9, [rsp+20h]
    call target
    RESTORE_GUEST_FS 40h
    add rsp, 0f8h
    ret
entry ENDP
ENDM

ANDROID_BUFFER_VARARGS AndroidVarargsFormat, StoneAndroidFormat
ANDROID_BUFFER_VARARGS AndroidVarargsWideFormat, StoneAndroidWideFormat

AndroidVarargsAllocateFormat PROC FRAME
    sub rsp, 0f8h
    .allocstack 0f8h
    .endprolog
    SAVE_VARARGS
    mov dword ptr [rsp+20h], 16
    mov rcx, rdi
    mov rdx, rsi
    lea r8, [rsp+20h]
    call StoneAndroidAllocateFormat
    RESTORE_GUEST_FS 40h
    add rsp, 0f8h
    ret
AndroidVarargsAllocateFormat ENDP

AndroidJniVarargs PROC FRAME
    sub rsp, 0f8h
    .allocstack 0f8h
    .endprolog
    SAVE_VARARGS
    mov dword ptr [rsp+20h], 24
    mov rcx, rdi
    mov r8, rdx
    mov rdx, rsi
    lea r9, [rsp+20h]
    call r10
    RESTORE_GUEST_FS 40h
    add rsp, 0f8h
    ret
AndroidJniVarargs ENDP

END
