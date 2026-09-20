NASM        ?= $(shell which nasm 2>/dev/null || echo /mnt/c/Users/chaha/AppData/Local/bin/NASM/nasm.exe)
CC          ?= gcc
LD          ?= ld
OBJCOPY     ?= objcopy
PYTHON      ?= python3
QEMU        ?= qemu-system-x86_64

BUILD_DIR   := build
SRC_DIR     := src
TOOLS_DIR   := tools

CFLAGS      := -m32 -ffreestanding -nostdlib -fno-pie -fno-stack-protector \
               -fno-builtin -Wall -Wextra -O2 -I$(SRC_DIR)/include
LDFLAGS     := -m elf_i386 -T $(SRC_DIR)/stage3/linker.ld -nostdlib

STAGE3_SRCS_C := $(SRC_DIR)/stage3/core/main.c \
                 $(SRC_DIR)/stage3/core/printf.c \
                 $(SRC_DIR)/stage3/debug/serial.c \
                 $(SRC_DIR)/stage3/debug/vga.c \
                 $(SRC_DIR)/stage3/debug/disk_log.c \
                 $(SRC_DIR)/stage3/bios/bios_disk.c \
                 $(SRC_DIR)/stage3/bios/vbe.c \
                 $(SRC_DIR)/stage3/memory/memory.c \
                 $(SRC_DIR)/stage3/pci/pci.c \
                 $(SRC_DIR)/stage3/xhci/xhci.c \
                 $(SRC_DIR)/stage3/usb/usb.c \
                 $(SRC_DIR)/stage3/usb/usb_msc.c \
                 $(SRC_DIR)/stage3/mtp/mtp.c \
                 $(SRC_DIR)/stage3/filesystem/fat_source.c \
                 $(SRC_DIR)/stage3/mtp/mtp_source.c \
                 $(SRC_DIR)/stage3/linux/linux_boot.c \
                 $(SRC_DIR)/stage3/image/image_detect.c \
                 $(SRC_DIR)/stage3/filesystem/iso_reader.c \
                 $(SRC_DIR)/stage3/image/os_scanner.c \
                 $(SRC_DIR)/stage3/adb/adb.c \
                 $(SRC_DIR)/stage3/ui/menu.c

STAGE3_SRCS_S := $(SRC_DIR)/stage3/core/entry.S \
                 $(SRC_DIR)/stage3/bios/bios_thunk.S

STAGE3_OBJS   := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(STAGE3_SRCS_C)) \
                 $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(STAGE3_SRCS_S))

all: $(BUILD_DIR)/boot.img

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/stage3/core
	mkdir -p $(BUILD_DIR)/stage3/debug
	mkdir -p $(BUILD_DIR)/stage3/bios
	mkdir -p $(BUILD_DIR)/stage3/memory
	mkdir -p $(BUILD_DIR)/stage3/pci

$(BUILD_DIR)/stage1.bin: $(SRC_DIR)/stage1/stage1.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

$(BUILD_DIR)/stage2.bin: $(SRC_DIR)/stage2/stage2.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/stage3.elf: $(STAGE3_OBJS)
	$(LD) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/stage3.bin: $(BUILD_DIR)/stage3.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/boot.img: $(BUILD_DIR)/stage1.bin $(BUILD_DIR)/stage2.bin $(BUILD_DIR)/stage3.bin $(TOOLS_DIR)/mkimage.py
	$(PYTHON) $(TOOLS_DIR)/mkimage.py \
		--stage1 $(BUILD_DIR)/stage1.bin \
		--stage2 $(BUILD_DIR)/stage2.bin \
		--stage3 $(BUILD_DIR)/stage3.bin \
		--output $@ \
		--size-mb 64

clean:
	rm -rf $(BUILD_DIR)

run: $(BUILD_DIR)/boot.img
	$(QEMU) -drive file=$(BUILD_DIR)/boot.img,format=raw,if=ide -serial stdio

run-xhci: $(BUILD_DIR)/boot.img
	$(QEMU) -drive file=$(BUILD_DIR)/boot.img,format=raw,if=ide -device qemu-xhci,id=xhci -serial stdio

.PHONY: all clean run run-xhci
