#include "keyboard.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"
#include "shell/shell.h"

extern void kputc(char c, uint32_t fg_color);
extern void kprint(const char *text, uint32_t fg_color);
extern void kbackspace(void);

#define KEY_QUEUE_SIZE 128U
#define KERNEL_LINE_SIZE 256U

static const char scancode_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
   '*',   0, ' '
};

static volatile char key_queue[KEY_QUEUE_SIZE];
static volatile uint16_t queue_head;
static volatile uint16_t queue_tail;
static volatile bool user_mode_active;
static bool shift_pressed;
static char kernel_line[KERNEL_LINE_SIZE];
static size_t kernel_line_length;

static uint64_t irq_save_disable(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void enqueue_key(char character) {
    uint16_t next = (uint16_t)((queue_head + 1U) % KEY_QUEUE_SIZE);
    if (next == queue_tail) {
        return;
    }
    key_queue[queue_head] = character;
    queue_head = next;
}

static bool dequeue_key(char *character) {
    if (queue_tail == queue_head) {
        return false;
    }
    *character = key_queue[queue_tail];
    queue_tail = (uint16_t)((queue_tail + 1U) % KEY_QUEUE_SIZE);
    return true;
}

void keyboard_init(void) {
    while (inb(0x64) & 2U) {}
    outb(0x64, 0xAE);

    while (inb(0x64) & 1U) {
        (void)inb(0x60);
    }
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    outb(0x20, 0x20);

    uint8_t code = scancode & 0x7FU;
    bool released = (scancode & 0x80U) != 0U;
    if (code == 0x2AU || code == 0x36U) {
        shift_pressed = !released;
        return;
    }
    if (released) {
        return;
    }

    char character = scancode_ascii[code];
    if (shift_pressed && character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    if (character != '\0') {
        enqueue_key(character);
    }
}

void keyboard_set_user_mode(bool active) {
    uint64_t flags = irq_save_disable();
    user_mode_active = active;
    queue_head = 0;
    queue_tail = 0;
    irq_restore(flags);
}

bool keyboard_read_user_char(char *character) {
    if (character == NULL) {
        return false;
    }

    for (;;) {
        __asm__ volatile ("cli" : : : "memory");
        if (!user_mode_active) {
            return false;
        }
        if (dequeue_key(character)) {
            return true;
        }
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
}

void keyboard_process_pending(void) {
    if (user_mode_active) {
        return;
    }

    char character;
    uint64_t flags = irq_save_disable();
    bool available = dequeue_key(&character);
    irq_restore(flags);
    if (!available) {
        return;
    }

    if (character == '\n') {
        kputc('\n', 0xFFFFFFU);
        kernel_line[kernel_line_length] = '\0';
        shell_handle_command(kernel_line);
        kernel_line_length = 0;
        kernel_line[0] = '\0';
        kprint("MyOS:> ", 0x00FFFFU);
    } else if (character == '\b') {
        if (kernel_line_length > 0U) {
            --kernel_line_length;
            kernel_line[kernel_line_length] = '\0';
            kbackspace();
        }
    } else if (character != '\0' && kernel_line_length < KERNEL_LINE_SIZE - 1U) {
        kernel_line[kernel_line_length++] = character;
        kernel_line[kernel_line_length] = '\0';
        kputc(character, 0x00FFFFU);
    }
}

void keyboard_wait_for_event(void) {
    __asm__ volatile ("cli" : : : "memory");
    if (queue_head == queue_tail) {
        __asm__ volatile ("sti; hlt" : : : "memory");
    } else {
        __asm__ volatile ("sti" : : : "memory");
    }
}
