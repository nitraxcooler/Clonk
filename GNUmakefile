# Pick the right toolchain for the host.
# MSYS2/clang64 on Windows gives us clean x86_64-elf objects; on Linux/WSL
# the system cc/ld works fine (both target the same bare-metal ELF format).
TARGET_TRIPLE = x86_64-elf

# Careful: use "=" here, not "?=". GNU make already defaults CC=cc and LD=ld,
# so "?=" would be a no-op and the Windows path would silently break.
ifeq ($(OS),Windows_NT)
CC = C:/msys64/clang64/bin/clang
LD = C:/msys64/clang64/bin/ld.lld
else
CC = cc
LD = ld
endif
PYTHON ?= python3

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

# Base CFLAGS shared by every compiler.
# The -mno-mmx/-mno-sse/-mno-80387 flags matter a lot: we never turn on
# SSE in CR0/CR4, so if the compiler emits any SIMD instructions (say, for
# a struct copy) the CPU will throw #UD and crash the kernel at runtime.
CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector \
         -fno-pie -fno-PIC -m64 -march=x86-64 -mno-red-zone -mcmodel=large \
         -mno-mmx -mno-sse -mno-sse2 -mno-80387 -msoft-float -I src
USER_CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector \
              -fno-pie -m64 -march=x86-64 -mno-red-zone -mcmodel=small \
              -mno-mmx -mno-sse -mno-sse2 -mno-80387 -msoft-float -I src

# Clang needs --target so it knows it's building bare-metal ELF for x86-64.
ifneq ($(findstring clang,$(CC)),)
CFLAGS += --target=$(TARGET_TRIPLE)
endif

LDFLAGS = -m elf_x86_64 -nostdlib -static -no-pie -T linker.lds

C_SOURCES = \
    src/kernel/main.c \
    src/kernel/pmm.c \
    src/kernel/vmm.c \
    src/kernel/elf.c \
    src/kernel/usermode.c \
    src/kernel/userfs.c \
    src/kernel/usercopy.c \
    src/kernel/heap.c \
    src/kernel/panic.c \
    src/kernel/blockdev.c \
    src/fs/vfs.c \
    src/fs/fat32.c \
    src/graphics/font.c \
    src/arch/x86_64/idt.c \
    src/arch/x86_64/gdt.c \
    src/drivers/keyboard/keyboard.c \
    src/drivers/storage/ramdisk.c \
    src/drivers/storage/ahci.c \
    src/drivers/pci/pci.c \
    src/lib/string.c \
    src/lib/mem.c \
    src/shell/shell.c

ASM_SOURCES = src/arch/x86_64/isr.s src/arch/x86_64/usermode.s

OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(C_SOURCES)) \
       $(patsubst src/%.s,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
DEPS = $(OBJS:.o=.d)

ISO_CONFIG = $(ISO_BOOT_DIR)/limine.conf
ISO_KERNEL = $(ISO_BOOT_DIR)/myos.elf
ISO_FATIMG = $(ISO_BOOT_DIR)/fat32.img
ISO_INIT = $(ISO_BOOT_DIR)/init.elf
USER_INIT = $(BUILD_DIR)/userspace/init.elf
FATIMG_SRC = tools/make_fat32_img.py
FATIMG = $(BUILD_DIR)/fat32.img
ISO_LIMINE_FILES = \
    $(ISO_BOOT_DIR)/limine-bios-cd.bin \
    $(ISO_BOOT_DIR)/limine-bios.sys \
    $(ISO_BOOT_DIR)/limine-uefi-cd.bin

.PHONY: all run run-ahci clean

all: $(ISO)

$(OBJ_DIR)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: src/%.s
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/userspace/init.o: src/userspace/init.c src/abi/syscall.h
	mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(if $(findstring clang,$(CC)),--target=$(TARGET_TRIPLE)) -c $< -o $@

$(USER_INIT): $(BUILD_DIR)/userspace/init.o src/userspace/linker.lds
	mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -static -no-pie -T src/userspace/linker.lds \
	    $(BUILD_DIR)/userspace/init.o -o $@

$(KERNEL): $(OBJS) linker.lds
	mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(ISO_CONFIG): iso_root/boot/limine.conf
	mkdir -p $(dir $@)
	cp $< $@

$(ISO_KERNEL): $(KERNEL)
	mkdir -p $(dir $@)
	cp $< $@

$(FATIMG): $(FATIMG_SRC)
	@mkdir -p $(BUILD_DIR)
	$(PYTHON) $(FATIMG_SRC)

$(ISO_FATIMG): $(FATIMG)
	@mkdir -p $(dir $@)
	cp $< $@

$(ISO_INIT): $(USER_INIT)
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

$(ISO): $(ISO_KERNEL) $(ISO_CONFIG) $(ISO_FATIMG) $(ISO_INIT) $(ISO_LIMINE_FILES) $(LIMINE_TOOL)
	mkdir -p $(dir $@)
	xorriso -as mkisofs -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(ISO_STAGE_DIR) -o $@
	$(LIMINE_TOOL) bios-install $@

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -display $(QEMU_DISPLAY) -serial $(QEMU_SERIAL) -monitor $(QEMU_MONITOR) -no-reboot

# Run with the FAT32 test image attached as a SATA disk on QEMU's ICH9
# AHCI controller (exercises the AHCI driver instead of the RAM disk).
run-ahci: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -boot order=d -display $(QEMU_DISPLAY) -serial $(QEMU_SERIAL) -monitor $(QEMU_MONITOR) -no-reboot \
	    -drive file=$(FATIMG),format=raw,if=none,id=disk0 \
	    -device ich9-ahci,id=ahci \
	    -device ide-hd,drive=disk0,bus=ahci.0

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
