#include "idt.h"
#include "io.h"
#include "kernel/panic.h"
#include "kernel/usermode.h"

static struct idt_entry idt[256];
static struct idt_ptr idtr;

extern void keyboard_isr_stub(void);
extern void syscall_isr_stub(void);
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void keyboard_init(void);

static const char *exception_names[32] = {
    "Divide Error",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception"
};

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low  = (uint16_t)(base & 0xFFFF);
    idt[num].selector    = sel;
    idt[num].ist         = 0;
    idt[num].attributes  = flags;
    idt[num].offset_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].zero        = 0;
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();

    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();

    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();

    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    outb(0x21, 0xFD);
    outb(0xA1, 0xFF);
}

void exception_handler_noerr(uint64_t *stack, uint64_t vector) {
    const char *name = "Unknown Exception";
    uint64_t *cpu_frame = stack + 15;
    uint64_t error_code = 0;
    uint64_t rip = cpu_frame[0];
    uint64_t cs = cpu_frame[1];
    uint64_t rflags = cpu_frame[2];
    uint64_t rsp = 0;
    uint64_t ss = 0;

    if ((cpu_frame[1] & 3U) == 3U) {
        user_process_fault(vector, rip, 0);
    }
    if (vector < 32) {
        name = exception_names[vector];
    }

    kernel_panic_exception(name, (uint8_t)vector, name, error_code, rip, cs, rflags, rsp, ss);
}

void exception_handler_err(uint64_t *stack, uint64_t vector) {
    const char *name = "Unknown Exception";
    uint64_t *cpu_frame = stack + 15;
    uint64_t error_code = cpu_frame[0];
    uint64_t rip = cpu_frame[1];
    uint64_t cs = cpu_frame[2];
    uint64_t rflags = cpu_frame[3];
    uint64_t rsp = 0;
    uint64_t ss = 0;
    uint64_t fault_address = 0;

    if ((cs & 3U) == 3U) {
        user_process_fault(vector, rip, error_code);
    }
    if (vector < 32) {
        name = exception_names[vector];
    }

    if (vector == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));

        cpu_reg_frame frame;
        frame.rax    = stack[14];
        frame.rbx    = stack[13];
        frame.rcx    = stack[12];
        frame.rdx    = stack[11];
        frame.rsi    = stack[10];
        frame.rdi    = stack[9];
        frame.rbp    = stack[8];
        frame.r8     = stack[7];
        frame.r9     = stack[6];
        frame.r10    = stack[5];
        frame.r11    = stack[4];
        frame.r12    = stack[3];
        frame.r13    = stack[2];
        frame.r14    = stack[1];
        frame.r15    = stack[0];
        frame.rip    = rip;
        frame.cs     = cs;
        frame.rflags = rflags;

        kernel_panic_page_fault("Page Fault", (uint8_t)vector, name, error_code, fault_address,
                                rip, cs, rflags, rsp, ss, &frame);
        return;
    }

    kernel_panic_exception(name, (uint8_t)vector, name, error_code, rip, cs, rflags, rsp, ss);
}

void idt_init(void) {
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    pic_remap();

    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    idt_set_gate(0, (uint64_t)isr0, cs, 0x8E);
    idt_set_gate(1, (uint64_t)isr1, cs, 0x8E);
    idt_set_gate(2, (uint64_t)isr2, cs, 0x8E);
    idt_set_gate(3, (uint64_t)isr3, cs, 0x8E);
    idt_set_gate(4, (uint64_t)isr4, cs, 0x8E);
    idt_set_gate(5, (uint64_t)isr5, cs, 0x8E);
    idt_set_gate(6, (uint64_t)isr6, cs, 0x8E);
    idt_set_gate(7, (uint64_t)isr7, cs, 0x8E);
    idt_set_gate(8, (uint64_t)isr8, cs, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, cs, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, cs, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, cs, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, cs, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, cs, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, cs, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, cs, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, cs, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, cs, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, cs, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, cs, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, cs, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, cs, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, cs, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, cs, 0x8E);
    idt_set_gate(0x21, (uint64_t)keyboard_isr_stub, cs, 0x8E);
    idt_set_gate(0x80, (uint64_t)syscall_isr_stub, cs, 0xEE);

    __asm__ volatile ("lidt %0" : : "m"(idtr));

    keyboard_init();

    __asm__ volatile ("sti");
}
