.text
.code64

.extern syscall_dispatch
.extern arch_saved_kernel_rsp
.extern arch_saved_kernel_rflags

.global arch_load_gdt
arch_load_gdt:
    lgdt (%rdi)
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    ret

.global arch_enter_user
arch_enter_user:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, arch_saved_kernel_rsp(%rip)
    pushfq
    popq arch_saved_kernel_rflags(%rip)

    movw $0x1b, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushq $0x1b
    pushq %rsi
    pushq $0x202
    pushq $0x23
    pushq %rdi
    iretq

.global arch_return_to_kernel
arch_return_to_kernel:
    movq arch_saved_kernel_rsp(%rip), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    pushq arch_saved_kernel_rflags(%rip)
    popfq
    ret

.global syscall_isr_stub
syscall_isr_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    call syscall_dispatch
    movq %rax, 112(%rsp)

    movw $0x1b, %ax
    movw %ax, %ds
    movw %ax, %es

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq
