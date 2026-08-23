#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "boot/limine.h"
#include "graphics/font.h"
#include "kernel/panic.h"
#include "arch/x86_64/io.h"

extern volatile uint64_t limine_base_revision[3];
extern volatile struct limine_framebuffer_request framebuffer_request;

static struct limine_framebuffer *panic_fb = NULL;

/* Minimal serial helpers for panics (COM1 0x3F8) */
static inline void panic_serial_putc(char c) { outb(0x3F8, (uint8_t)c); }
static void panic_serial_write(const char *s) { while (*s) panic_serial_putc(*s++); }
static void panic_serial_hex64(uint64_t v) {
    const char *digits = "0123456789ABCDEF";
    panic_serial_write("0x");
    for (int i = 15; i >= 0; --i) {
        uint8_t nibble = (v >> (i * 4)) & 0xF;
        panic_serial_putc(digits[nibble]);
    }
}

static void panic_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if ((unsigned char)c > 127) {
        return;
    }
    volatile uint32_t *fb_ptr = (volatile uint32_t *)panic_fb->address;
    uint32_t pitch_pixels = panic_fb->pitch / 4;

    for (int row = 0; row < 16; row++) {
        uint8_t glyph_row = font8x16[(unsigned char)c][row];
        for (int col = 0; col < 8; col++) {
            uint32_t pixel_color = (glyph_row & (1u << (7 - col))) ? fg_color : bg_color;
            fb_ptr[(y + row) * pitch_pixels + (x + col)] = pixel_color;
        }
    }
}

static void panic_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    uint32_t cx = x;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            y += 18;
        } else {
            panic_draw_char(*str, cx, y, fg_color, bg_color);
            cx += 8;
        }
        str++;
    }
}

static void panic_draw_centered(const char *str, uint32_t y, uint32_t fg_color) {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }

    uint32_t x = (panic_fb->width / 2) - ((uint32_t)len * 8 / 2);
    panic_draw_string(str, x, y, fg_color, 0x000000);
}

static void panic_draw_hex64(uint64_t value, uint32_t x, uint32_t y, uint32_t color) {
    static const char digits[] = "0123456789ABCDEF";
    char buf[19];
    size_t i = 0;

    for (int shift = 60; shift >= 0; shift -= 4) {
        buf[i++] = digits[(value >> shift) & 0xF];
    }
    buf[i] = '\0';
    panic_draw_string("0x", x, y, color, 0x000000);
    panic_draw_string(buf, x + 16, y, color, 0x000000);
}

static void panic_fill_screen(uint32_t color) {
    if (panic_fb == NULL) {
        return;
    }

    volatile uint32_t *fb_ptr = (volatile uint32_t *)panic_fb->address;
    uint32_t pixels = (panic_fb->pitch / 4) * panic_fb->height;
    for (uint32_t i = 0; i < pixels; i++) {
        fb_ptr[i] = color;
    }
}

static void panic_render_header(const char *reason) {
    (void)reason;
    panic_draw_centered("KERNEL PANIC", 28, 0xFFFFFF);

    panic_draw_string("MyOS has encountered a fatal kernel exception.", 32, 72, 0xFFFFFF, 0x000000);
    panic_draw_string("Reason: ", 32, 110, 0xFFDDDD, 0x000000);
    panic_draw_string(reason, 32 + 8 * 8, 110, 0xFFFFFF, 0x000000);
}

static void panic_render_exception_details(const char *name, uint8_t vector,
                                         uint64_t error_code, uint64_t rip,
                                         uint64_t cs, uint64_t rflags,
                                         uint64_t rsp, uint64_t ss) {
    panic_draw_string("Exception: ", 32, 150, 0xFFD5D5, 0x000000);
    panic_draw_string(name, 32 + 11 * 8, 150, 0xFFFFFF, 0x000000);

    panic_draw_string("Vector: ", 32, 168, 0xFFD5D5, 0x000000);
    panic_draw_hex64(vector, 32 + 9 * 8, 168, 0xFFFFFF);

    panic_draw_string("Error Code: ", 32, 186, 0xFFD5D5, 0x000000);
    panic_draw_hex64(error_code, 32 + 13 * 8, 186, 0xFFFFFF);

    panic_draw_string("RIP: ", 32, 214, 0xFFD5D5, 0x000000);
    panic_draw_hex64(rip, 32 + 7 * 8, 214, 0xFFFFFF);

    panic_draw_string("CS: ", 32, 232, 0xFFD5D5, 0x000000);
    panic_draw_hex64(cs, 32 + 6 * 8, 232, 0xFFFFFF);

    panic_draw_string("RFLAGS: ", 32, 250, 0xFFD5D5, 0x000000);
    panic_draw_hex64(rflags, 32 + 10 * 8, 250, 0xFFFFFF);

    if (rsp != 0 || ss != 0) {
        panic_draw_string("RSP: ", 32, 268, 0xFFD5D5, 0x000000);
        panic_draw_hex64(rsp, 32 + 7 * 8, 268, 0xFFFFFF);
        panic_draw_string("SS: ", 32, 286, 0xFFD5D5, 0x000000);
        panic_draw_hex64(ss, 32 + 6 * 8, 286, 0xFFFFFF);
    }

    panic_draw_string("System halted. Manual restart required.", 32, 330, 0xFFEEEE, 0x000000);
}

void decode_page_error(uint64_t error, pf_decode *out) {
    if (out == NULL) {
        return;
    }
    out->present     = (error & 0x1ULL) != 0;
    out->write       = (error & 0x2ULL) != 0;
    out->user        = (error & 0x4ULL) != 0;
    out->reserved    = (error & 0x8ULL) != 0;
    out->instruction = (error & 0x10ULL) != 0;
}

static void panic_register_value(uint32_t row, uint32_t col, const char *name, uint64_t value) {
    panic_draw_string(name, col, row, 0xFFD5D5, 0x000000);
    panic_draw_hex64(value, col + 5 * 8, row, 0xFFFFFF);
}

static void panic_render_page_fault(uint64_t fault_address, uint64_t error_code,
                                  uint64_t rip, uint64_t cs, uint64_t rflags,
                                  uint64_t rsp, uint64_t ss,
                                  const cpu_reg_frame *regs) {
    pf_decode d;
    decode_page_error(error_code, &d);

    panic_draw_string("Fault Address: ", 32, 200, 0xFFD5D5, 0x000000);
    panic_draw_hex64(fault_address, 32 + 15 * 8, 200, 0xFFFFFF);

    panic_draw_string("RIP:    ", 32, 218, 0xFFD5D5, 0x000000);
    panic_draw_hex64(rip, 32 + 9 * 8, 218, 0xFFFFFF);
    panic_draw_string("CS:     ", 32 + 26 * 8, 218, 0xFFD5D5, 0x000000);
    panic_draw_hex64(cs, 32 + 30 * 8, 218, 0xFFFFFF);

    panic_draw_string("RSP:    ", 32, 236, 0xFFD5D5, 0x000000);
    panic_draw_hex64(rsp, 32 + 9 * 8, 236, 0xFFFFFF);
    panic_draw_string("RFLAGS: ", 32 + 26 * 8, 236, 0xFFD5D5, 0x000000);
    panic_draw_hex64(rflags, 32 + 35 * 8, 236, 0xFFFFFF);

    panic_draw_string("Error Code: ", 32, 254, 0xFFD5D5, 0x000000);
    panic_draw_hex64(error_code, 32 + 13 * 8, 254, 0xFFFFFF);

    /* Decoded page-fault error bits (x86-64 CR2 / error-code semantics). */
    panic_draw_string("Present: ", 32, 282, 0xFFD5D5, 0x000000);
    panic_draw_string(d.present ? "Yes" : "No", 32 + 9 * 8, 282, 0xFFFFFF, 0x000000);
    panic_draw_string("Write: ", 32 + 20 * 8, 282, 0xFFD5D5, 0x000000);
    panic_draw_string(d.write ? "Yes" : "No", 32 + 27 * 8, 282, 0xFFFFFF, 0x000000);
    panic_draw_string("User: ", 32 + 38 * 8, 282, 0xFFD5D5, 0x000000);
    panic_draw_string(d.user ? "Yes" : "No", 32 + 44 * 8, 282, 0xFFFFFF, 0x000000);

    panic_draw_string("Reserved: ", 32, 300, 0xFFD5D5, 0x000000);
    panic_draw_string(d.reserved ? "Yes" : "No", 32 + 10 * 8, 300, 0xFFFFFF, 0x000000);
    panic_draw_string("Instruction: ", 32 + 24 * 8, 300, 0xFFD5D5, 0x000000);
    panic_draw_string(d.instruction ? "Yes" : "No", 32 + 37 * 8, 300, 0xFFFFFF, 0x000000);

    /* Explain what the decoded state means. */
    if (d.reserved) {
        panic_draw_string("Type: reserved-bit violation", 32, 318, 0xFFEEEE, 0x000000);
    } else if (!d.present) {
        panic_draw_string("Type: page not present", 32, 318, 0xFFEEEE, 0x000000);
    } else {
        panic_draw_string("Type: protection violation (page present)", 32, 318, 0xFFEEEE, 0x000000);
    }

    /* Full register snapshot captured by the ISR. */
    panic_draw_string("CPU register snapshot at fault:", 32, 352, 0xFFDDDD, 0x000000);

            /* Left column and right column for the 15 general-purpose registers.
     * Left label+value spans up to col+184; keep the right column far
     * enough to avoid text overlap. */
    const uint32_t lx = 32;
    const uint32_t rx = 32 + 36 * 8;   /* right-hand column */

    panic_register_value(374, lx, "RAX  ", regs ? regs->rax : 0);
    panic_register_value(374, rx, "RBX  ", regs ? regs->rbx : 0);
    panic_register_value(392, lx, "RCX  ", regs ? regs->rcx : 0);
    panic_register_value(392, rx, "RDX  ", regs ? regs->rdx : 0);
    panic_register_value(410, lx, "RSI  ", regs ? regs->rsi : 0);
    panic_register_value(410, rx, "RDI  ", regs ? regs->rdi : 0);
    panic_register_value(428, lx, "RBP  ", regs ? regs->rbp : 0);
    panic_register_value(428, rx, "R8   ", regs ? regs->r8 : 0);
    panic_register_value(446, lx, "R9   ", regs ? regs->r9 : 0);
    panic_register_value(446, rx, "R10  ", regs ? regs->r10 : 0);
    panic_register_value(464, lx, "R11  ", regs ? regs->r11 : 0);
    panic_register_value(464, rx, "R12  ", regs ? regs->r12 : 0);
    panic_register_value(482, lx, "R13  ", regs ? regs->r13 : 0);
    panic_register_value(482, rx, "R14  ", regs ? regs->r14 : 0);
    panic_register_value(500, lx, "R15  ", regs ? regs->r15 : 0);

    panic_draw_string("System halted. Manual restart required.", 32, 552, 0xFFEEEE, 0x000000);
}

void kernel_panic_exception(const char *reason, uint8_t vector, const char *name,
                           uint64_t error_code, uint64_t rip, uint64_t cs,
                           uint64_t rflags, uint64_t rsp, uint64_t ss) {
    /* Emit minimal serial diagnostics first so headless boots capture the panic */
    panic_serial_write("PANIC: ");
    panic_serial_write(reason);
    panic_serial_write("\nVector: ");
    panic_serial_hex64((uint64_t)vector);
    panic_serial_write(" Error: ");
    panic_serial_hex64(error_code);
    panic_serial_write(" RIP: ");
    panic_serial_hex64(rip);
    panic_serial_write("\n");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    panic_fb = framebuffer_request.response->framebuffers[0];
    panic_fill_screen(0x880000);
    panic_render_header(reason);
    panic_render_exception_details(name, vector, error_code, rip, cs, rflags, rsp, ss);

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_panic_page_fault(const char *reason, uint8_t vector, const char *name,
                            uint64_t error_code, uint64_t fault_address,
                            uint64_t rip, uint64_t cs, uint64_t rflags,
                            uint64_t rsp, uint64_t ss,
                            const cpu_reg_frame *regs) {
    /* Emit serial diagnostics early */
    panic_serial_write("PANIC: ");
    panic_serial_write(reason);
    panic_serial_write("\nFault addr: ");
    panic_serial_hex64(fault_address);
    panic_serial_write(" Error: ");
    panic_serial_hex64(error_code);
    panic_serial_write(" RIP: ");
    panic_serial_hex64(rip);
    panic_serial_write("\n");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    panic_fb = framebuffer_request.response->framebuffers[0];
    panic_fill_screen(0x880000);
    panic_render_header(reason);
    panic_draw_string("Exception: ", 32, 150, 0xFFD5D5, 0x000000);
    panic_draw_string(name, 32 + 11 * 8, 150, 0xFFFFFF, 0x000000);
    panic_draw_string("Vector: ", 32, 168, 0xFFD5D5, 0x000000);
    panic_draw_hex64(vector, 32 + 9 * 8, 168, 0xFFFFFF);
    panic_render_page_fault(fault_address, error_code, rip, cs, rflags, rsp, ss, regs);

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_panic(const char *reason) {
    kernel_panic_exception(reason, 0, "Kernel Panic", 0, 0, 0, 0, 0, 0);
}
