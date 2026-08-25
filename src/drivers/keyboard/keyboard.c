#include <stdint.h>
#include "arch/x86_64/io.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "lib/string.h"
#include "shell/shell.h"

extern void kprint(const char *str, uint32_t fg_color);
extern void kputc(char c, uint32_t fg_color);
extern void kbackspace(void);
extern void kclear(void);

#define CMD_BUFFER_SIZE 256
static char cmd_buffer[CMD_BUFFER_SIZE];
static size_t cmd_len = 0;

static const char scancode_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
   '*',   0, ' '
};

void keyboard_init(void) {
    while (inb(0x64) & 2);
    outb(0x64, 0xAE); 

    while (inb(0x64) & 1) {
        inb(0x60);
    }
}

static void parse_command(const char *cmd) {
    shell_handle_command(cmd);
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    outb(0x20, 0x20);

    if (!(scancode & 0x80)) { 
        char ch = scancode_ascii[scancode];

        if (ch == '\n') {
            kputc('\n', 0xFFFFFF);
            cmd_buffer[cmd_len] = '\0';
            
            parse_command(cmd_buffer);

            cmd_len = 0;
            cmd_buffer[0] = '\0';
            
            kprint("MyOS:> ", 0x00FFFF);
        } else if (ch == '\b') {
            if (cmd_len > 0) {
                cmd_len--;
                cmd_buffer[cmd_len] = '\0';
                kbackspace();
            }
        } else if (ch != 0) {
            if (cmd_len < CMD_BUFFER_SIZE - 1) {
                cmd_buffer[cmd_len++] = ch;
                cmd_buffer[cmd_len] = '\0';
                kputc(ch, 0x00FFFF);
            }
        }
    }
}
