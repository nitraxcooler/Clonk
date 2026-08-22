# Use the MSYS/clang64 toolchain if present to reliably produce x86_64-elf ELF objects
CC = clang
LD = ld.lld
TARGET_TRIPLE = x86_64-elf

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
ISO_STAGE_DIR = $(BUILD_DIR)/iso_root
ISO_BOOT_DIR = $(ISO_STAGE_DIR)/boot
KERNEL = $(BUILD_DIR)/myos.elf
ISO = $(BUILD_DIR)/myos.iso
LIMINE_DIR = third_party/limine
ifeq ($(OS),Windows_NT)
LIMINE_TOOL = $(LIMINE_DIR)/limine.exe
else
LIMINE_TOOL = $(LIMINE_DIR)/limine
endif

ifneq ($(DISPLAY)$(WAYLAND_DISPLAY),)
QEMU_DISPLAY ?= gtk
else
QEMU_DISPLAY ?= none
endif
QEMU_SERIAL ?= stdio
QEMU_MONITOR ?= none

# Base CFLAGS shared across all compilers
CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector \
         -fno-pie -fno-PIC -m64 -march=x86-64 -mno-red-zone -mcmodel=large -I src

# Append --target only when using Clang
ifneq ($(findstring clang,$(CC)),)
CFLAGS += --target=$(TARGET_TRIPLE)
endif

LDFLAGS = -m elf_x86_64 -nostdlib -static -no-pie -T linker.lds

C_SOURCES = \
    src/kernel/main.c \
    src/kernel/pmm.c \
    src/kernel/vmm.c \
    src/kernel/panic.c \
    src/graphics/font.c \
    src/arch/x86_64/idt.c \
    src/drivers/keyboard/keyboard.c \
    src/lib/string.c

ASM_SOURCES = src/arch/x86_64/isr.s

OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(C_SOURCES)) \
       $(patsubst src/%.s,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
DEPS = $(OBJS:.o=.d)

ISO_CONFIG = $(ISO_BOOT_DIR)/limine.conf
ISO_KERNEL = $(ISO_BOOT_DIR)/myos.elf
ISO_LIMINE_FILES = \
    $(ISO_BOOT_DIR)/limine-bios-cd.bin \
    $(ISO_BOOT_DIR)/limine-bios.sys \
    $(ISO_BOOT_DIR)/limine-uefi-cd.bin

.PHONY: all run clean

all: $(ISO)

$(OBJ_DIR)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: src/%.s
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJS) linker.lds
	mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(ISO_CONFIG): iso_root/boot/limine.conf
	mkdir -p $(dir $@)
	cp $< $@

$(ISO_KERNEL): $(KERNEL)
	mkdir -p $(dir $@)
	cp $< $@

$(ISO_BOOT_DIR)/limine-bios-cd.bin: $(LIMINE_DIR)/limine-bios-cd.bin
	mkdir -p $(dir $@)
	cp $< $@

$(ISO_BOOT_DIR)/limine-bios.sys: $(LIMINE_DIR)/limine-bios.sys
	mkdir -p $(dir $@)
	cp $< $@

$(ISO_BOOT_DIR)/limine-uefi-cd.bin: $(LIMINE_DIR)/limine-uefi-cd.bin
	mkdir -p $(dir $@)
	cp $< $@

$(LIMINE_TOOL):
	$(MAKE) -C $(LIMINE_DIR)

$(ISO): $(ISO_KERNEL) $(ISO_CONFIG) $(ISO_LIMINE_FILES) $(LIMINE_TOOL)
	mkdir -p $(dir $@)
	xorriso -as mkisofs -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(ISO_STAGE_DIR) -o $@
	$(LIMINE_TOOL) bios-install $@

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -display $(QEMU_DISPLAY) -serial $(QEMU_SERIAL) -monitor $(QEMU_MONITOR) -no-reboot

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
