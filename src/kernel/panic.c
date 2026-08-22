#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "boot/limine.h"
#include "graphics/font.h"
#include "kernel/panic.h"
#include "arch/x86_64/io.h"
#include "lib/string.h"

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

static void panic_render_page_fault(uint64_t fault_address, uint64_t error_code,
                                  uint64_t rip, uint64_t cs, uint64_t rflags,
                                  uint64_t rsp, uint64_t ss) {
    panic_draw_string("Fault address: ", 32, 186, 0xFFD5D5, 0x000000);
    panic_draw_hex64(fault_address, 32 + 16 * 8, 186, 0xFFFFFF);
    panic_draw_string("Error code: ", 32, 204, 0xFFD5D5, 0x000000);
    panic_draw_hex64(error_code, 32 + 13 * 8, 204, 0xFFFFFF);

    /* Decode the mandatory page-fault error-code bits:
     *  bit 0 (P)  0 = not present, 1 = protection violation
     *  bit 1 (W/R) 0 = read,       1 = write
     *  bit 2 (U/S) 0 = supervisor, 1 = user                  */
    const char *what = ((error_code & 0x1U) != 0U) ? "protection violation" : "not present";
    const char *op   = ((error_code & 0x2U) != 0U) ? "write" : "read";
    const char *who  = ((error_code & 0x4U) != 0U) ? "user" : "supervisor";

    panic_draw_string("Type: ", 32, 222, 0xFFD5D5, 0x000000);
    panic_draw_string(what, 32 + 7 * 8, 222, 0xFFFFFF, 0x000000);
    panic_draw_string("Access: ", 32, 240, 0xFFD5D5, 0x000000);
    panic_draw_string(op, 32 + 9 * 8, 240, 0xFFFFFF, 0x000000);
    panic_draw_string(" ", 32 + 9 * 8 + (uint32_t)(strlen(op) * 8), 240, 0xFFFFFF, 0x000000);
    panic_draw_string("by ", 32 + 9 * 8 + (uint32_t)(strlen(op) * 8) + 8, 240, 0xFFFFFF, 0x000000);
    panic_draw_string(who, 32 + 9 * 8 + (uint32_t)(strlen(op) * 8) + 40, 240, 0xFFFFFF, 0x000000);

    panic_draw_string("RIP: ", 32, 258, 0xFFD5D5, 0x000000);
    panic_draw_hex64(rip, 32 + 7 * 8, 258, 0xFFFFFF);

    panic_draw_string("CS: ", 32, 276, 0xFFD5D5, 0x000000);
    panic_draw_hex64(cs, 32 + 6 * 8, 276, 0xFFFFFF);

    panic_draw_string("RFLAGS: ", 32, 294, 0xFFD5D5, 0x000000);
    panic_draw_hex64(rflags, 32 + 10 * 8, 294, 0xFFFFFF);

    if (rsp != 0 || ss != 0) {
        panic_draw_string("RSP: ", 32, 312, 0xFFD5D5, 0x000000);
        panic_draw_hex64(rsp, 32 + 7 * 8, 312, 0xFFFFFF);
        panic_draw_string("SS: ", 32, 330, 0xFFD5D5, 0x000000);
        panic_draw_hex64(ss, 32 + 6 * 8, 330, 0xFFFFFF);
    }
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
                            uint64_t rsp, uint64_t ss) {
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
    panic_render_page_fault(fault_address, error_code, rip, cs, rflags, rsp, ss);
    panic_draw_string("System halted. Manual restart required.", 32, 330, 0xFFEEEE, 0x000000);

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_panic(const char *reason) {
    kernel_panic_exception(reason, 0, "Kernel Panic", 0, 0, 0, 0, 0, 0);
}
