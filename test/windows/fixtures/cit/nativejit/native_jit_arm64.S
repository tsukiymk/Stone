.text
.globl JNI_OnLoad
.type JNI_OnLoad,%function
JNI_OnLoad:
    stp x29, x30, [sp, #-32]!
    add x1, sp, #16
    movz w2, #6
    movk w2, #1, lsl #16
    ldr x8, [x0]
    ldr x8, [x8, #48] // JavaVM::GetEnv
    blr x8
    cbnz w0, .Lbad_version
    ldr x0, [sp, #16]
    ldr x8, [x0]
    ldr x8, [x8, #48] // JNIEnv::FindClass
    adrp x1, .Lclass_name
    add x1, x1, :lo12:.Lclass_name
    blr x8
    cbz x0, .Lbad_version
    mov x1, x0
    ldr x0, [sp, #16]
    ldr x8, [x0]
    ldr x8, [x8, #1720] // JNIEnv::RegisterNatives
    adrp x2, .Lmethods
    add x2, x2, :lo12:.Lmethods
    mov w3, #4
    blr x8
    cbnz w0, .Lbad_version
    mrs x8, tpidr_el0
    ldr x8, [x8, #40]
    cbz x8, .Lbad_version
    adrp x8, .Linitialized
    ldr w8, [x8, :lo12:.Linitialized]
    cbz w8, .Lbad_version
    adrp x0, .Linitialized
    add x0, x0, :lo12:.Linitialized
    bl __cxa_finalize
    adrp x8, .Linitialized
    ldr w8, [x8, :lo12:.Linitialized]
    cmp w8, #2
    b.ne .Lbad_version
    movz w0, #6
    movk w0, #1, lsl #16
    ldp x29, x30, [sp], #32
    ret
.Lbad_version:
    mov w0, #0
    ldp x29, x30, [sp], #32
    ret
.size JNI_OnLoad, .-JNI_OnLoad

.globl Java_com_trebledot_stone_cit_nativejit_JitActivity_add
.type Java_com_trebledot_stone_cit_nativejit_JitActivity_add,%function
Java_com_trebledot_stone_cit_nativejit_JitActivity_add:
    add w0, w2, w3
    add w0, w0, #21
    ret
.size Java_com_trebledot_stone_cit_nativejit_JitActivity_add, .-Java_com_trebledot_stone_cit_nativejit_JitActivity_add

.globl Java_com_trebledot_stone_cit_nativejit_JitActivity_addWide
.type Java_com_trebledot_stone_cit_nativejit_JitActivity_addWide,%function
Java_com_trebledot_stone_cit_nativejit_JitActivity_addWide:
    sxtw x0, w3
    add x0, x2, x0
    ret
.size Java_com_trebledot_stone_cit_nativejit_JitActivity_addWide, .-Java_com_trebledot_stone_cit_nativejit_JitActivity_addWide

.globl Java_com_trebledot_stone_cit_nativejit_JitActivity_mix
.type Java_com_trebledot_stone_cit_nativejit_JitActivity_mix,%function
Java_com_trebledot_stone_cit_nativejit_JitActivity_mix:
    add x0, x2, x3, lsl #1
    sub x4, x4, x5
    eor x0, x0, x4
    ret
.size Java_com_trebledot_stone_cit_nativejit_JitActivity_mix, .-Java_com_trebledot_stone_cit_nativejit_JitActivity_mix

.globl Java_com_trebledot_stone_cit_nativejit_JitActivity_zeroExtend
.type Java_com_trebledot_stone_cit_nativejit_JitActivity_zeroExtend,%function
Java_com_trebledot_stone_cit_nativejit_JitActivity_zeroExtend:
    mov w0, w2
    ret
.size Java_com_trebledot_stone_cit_nativejit_JitActivity_zeroExtend, .-Java_com_trebledot_stone_cit_nativejit_JitActivity_zeroExtend

.LregisteredLeaf:
    add w0, w2, #7
    ret

.LregisteredString:
    stp x29, x30, [sp, #-112]!
    str x19, [sp, #96]
    mov x19, x0
    adr x8, .Lstring_value
    mov w9, #42
    stp x8, x9, [sp, #48]
    add x8, sp, #112 // __stack
    add x9, sp, #64 // __gr_top
    stp x8, x9, [sp, #64]
    movn w8, #15 // __gr_offs = -16, __vr_offs = 0
    stp xzr, x8, [sp, #80]
    add x0, sp, #16
    mov w1, #32
    adr x2, .Lstring_format
    add x3, sp, #64
    bl vsnprintf
    mov x0, x19
    ldr x8, [x19]
    ldr x8, [x8, #1336] // JNIEnv::NewStringUTF
    add x1, sp, #16
    blr x8
    ldr x19, [sp, #96]
    ldp x29, x30, [sp], #112
    ret

.LregisteredDouble:
    stp q0, q1, [sp, #-32]!
    ldp q0, q1, [sp], #32
    fmov w8, s0
    movz w9, #0x3fc0, lsl #16 // 1.5f
    cmp w8, w9
    b.ne .Lwrong_float
    fmov d0, d1
    ret
.Lwrong_float:
    fmov d0, d0
    ret

.LregisteredStack:
    ldar x8, [sp]
    stp x29, x30, [sp, #-32]!
    add x9, x2, x8
    str x9, [sp, #16]
    add x10, sp, #28
    mov w11, #37
    stlr w11, [x10]
    ldar w12, [x10]
    cmp w11, w12
    b.ne .Lfailed_once
    adrp x0, .Lonce_state
    add x0, x0, :lo12:.Lonce_state
    adr x1, .Lonce_init
    bl pthread_once
    cbnz w0, .Lfailed_once
    adrp x8, .Lonce_counter
    add x8, x8, :lo12:.Lonce_counter
    ldar x8, [x8]
    cmp x8, #1
    b.ne .Lfailed_once
    ldr x0, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.Lfailed_once:
    mov x0, #0
    ldp x29, x30, [sp], #32
    ret
.Lonce_init:
    adrp x8, .Lonce_counter
    add x8, x8, :lo12:.Lonce_counter
    ldar x9, [x8]
    add x9, x9, #1
    stlr x9, [x8]
    ret

.Linit:
    stp x29, x30, [sp, #-32]!
    str x19, [sp, #24]
    mov x29, sp
    mov w8, #31
    msr fpsr, x8
    add x0, sp, #16
    bl feholdexcept
    cbnz w0, .Lreturn
    mrs x8, fpsr
    cbnz x8, .Lreturn
    ldr w8, [sp, #20]
    cmp w8, #31
    b.ne .Lreturn
    add x0, sp, #16
    bl fesetenv
    cbnz w0, .Lreturn
    mrs x8, fpsr
    cmp x8, #31
    b.ne .Lreturn
    mov x8, #3
    dup v0.2d, x8
    adrp x9, .Lshifts
    ldr q1, [x9, :lo12:.Lshifts]
    ushl v0.2d, v0.2d, v1.2d
    ext v1.16b, v0.16b, v0.16b, #8
    orr v0.8b, v0.8b, v1.8b
    fmov x8, d0
    cmp x8, #7
    b.ne .Lreturn
    adrp x19, .Linitialized
    add x19, x19, :lo12:.Linitialized
    adr x0, .Ldestroy
    mov x1, x19
    mov x2, x19
    bl __cxa_atexit
    cbnz w0, .Lreturn
    mov w9, #1
    str w9, [x19]
.Lreturn:
    msr fpsr, xzr
    ldr x19, [sp, #24]
    ldp x29, x30, [sp], #32
    ret

.Ldestroy:
    ldr w8, [x0]
    add w8, w8, #1
    str w8, [x0]
    ret

.section .rodata,"a",%progbits
.p2align 4
.Lshifts:
    .quad 1, -1
.Lclass_name:
    .asciz "com/trebledot/stone/cit/nativejit/JitActivity"
.Lstring_value:
    .asciz "arm64 registered"
.Lstring_format:
    .asciz "%s %d"
.Lleaf_name:
    .asciz "registeredLeaf"
.Lleaf_sig:
    .asciz "(I)I"
.Lstring_name:
    .asciz "registeredString"
.Lstring_sig:
    .asciz "()Ljava/lang/String;"
.Ldouble_name:
    .asciz "registeredDouble"
.Ldouble_sig:
    .asciz "(FID)D"
.Lstack_name:
    .asciz "registeredStack"
.Lstack_sig:
    .asciz "(JJJJJJJ)J"

.bss
.p2align 2
.Linitialized:
    .zero 4
.Lonce_state:
    .zero 4
.p2align 3
.Lonce_counter:
    .zero 8

.section .init_array,"aw",%init_array
    .quad .Linit

# Exercise relocation against the Windows host bridge during ELF loading.
.section .data.rel.ro,"aw",%progbits
.p2align 3
.Lmethods:
    .quad .Lleaf_name, .Lleaf_sig, .LregisteredLeaf
    .quad .Lstring_name, .Lstring_sig, .LregisteredString
    .quad .Ldouble_name, .Ldouble_sig, .LregisteredDouble
    .quad .Lstack_name, .Lstack_sig, .LregisteredStack
    .quad dlclose

.section .note.GNU-stack,"",%progbits
