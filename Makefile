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
CFLAGS_RELEASE := $(CFLAGS)
CFLAGS_DEBUG   := $(CFLAGS) -DDEBUG_BUILD=1
LDFLAGS        := -m elf_i386 -T $(SRC_DIR)/stage3/linker.ld -nostdlib
LIBGCC         := $(shell $(CC) -m32 -print-libgcc-file-name 2>/dev/null)

STAGE3_SRCS_C := $(SRC_DIR)/stage3/core/main.c \
                 $(SRC_DIR)/stage3/core/printf.c \
                 $(SRC_DIR)/stage3/core/timer.c \
                 $(SRC_DIR)/stage3/debug/serial.c \
                 $(SRC_DIR)/stage3/debug/vga.c \
                 $(SRC_DIR)/stage3/debug/disk_log.c \
                 $(SRC_DIR)/stage3/bios/bios_disk.c \
                 $(SRC_DIR)/stage3/bios/vbe.c \
                 $(SRC_DIR)/stage3/memory/heap.c \
                 $(SRC_DIR)/stage3/memory/memory.c \
                 $(SRC_DIR)/stage3/pci/pci.c \
                 $(SRC_DIR)/stage3/xhci/xhci.c \
                 $(SRC_DIR)/stage3/usb/usb.c \
                 $(SRC_DIR)/stage3/usb/usb_msc.c \
                 $(SRC_DIR)/stage3/mtp/mtp.c \
                 $(SRC_DIR)/stage3/filesystem/ff.c \
                 $(SRC_DIR)/stage3/filesystem/diskio.c \
                 $(SRC_DIR)/stage3/filesystem/fat_source.c \
                 $(SRC_DIR)/stage3/mtp/mtp_source.c \
                 $(SRC_DIR)/stage3/linux/linux_boot.c \
                 $(SRC_DIR)/stage3/image/image_detect.c \
                 $(SRC_DIR)/stage3/filesystem/iso_reader.c \
                 $(SRC_DIR)/stage3/filesystem/boot_cfg_parser.c \
                 $(SRC_DIR)/stage3/image/os_scanner.c \
                 $(SRC_DIR)/stage3/adb/adb.c \
                 $(SRC_DIR)/stage3/ui/menu.c

STAGE3_SRCS_S := $(SRC_DIR)/stage3/core/entry.S \
                 $(SRC_DIR)/stage3/bios/bios_thunk.S

STAGE3_OBJS_RELEASE := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/release/%.o, $(STAGE3_SRCS_C)) \
                       $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/release/%.o, $(STAGE3_SRCS_S))
STAGE3_OBJS_DEBUG   := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/debug/%.o, $(STAGE3_SRCS_C)) \
                       $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/debug/%.o, $(STAGE3_SRCS_S))

all: $(BUILD_DIR)/boot.img $(BUILD_DIR)/boot-debug.img

release: $(BUILD_DIR)/boot.img
prod: release
debug: $(BUILD_DIR)/boot-debug.img

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/stage1.bin: $(SRC_DIR)/stage1/stage1.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

$(BUILD_DIR)/stage2.bin: $(SRC_DIR)/stage2/stage2.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

STAGE3_HEADERS := $(wildcard $(SRC_DIR)/include/*.h) $(wildcard $(SRC_DIR)/stage3/*/*.h)

$(BUILD_DIR)/release/%.o: $(SRC_DIR)/%.c $(STAGE3_HEADERS) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RELEASE) -c $< -o $@

$(BUILD_DIR)/release/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RELEASE) -c $< -o $@

$(BUILD_DIR)/debug/%.o: $(SRC_DIR)/%.c $(STAGE3_HEADERS) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -c $< -o $@

$(BUILD_DIR)/debug/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -c $< -o $@

$(BUILD_DIR)/stage3.elf: $(STAGE3_OBJS_RELEASE)
	$(LD) $(LDFLAGS) $^ $(LIBGCC) -o $@

$(BUILD_DIR)/stage3.bin: $(BUILD_DIR)/stage3.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/stage3-debug.elf: $(STAGE3_OBJS_DEBUG)
	$(LD) $(LDFLAGS) $^ $(LIBGCC) -o $@

$(BUILD_DIR)/stage3-debug.bin: $(BUILD_DIR)/stage3-debug.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/boot.img: $(BUILD_DIR)/stage1.bin $(BUILD_DIR)/stage2.bin $(BUILD_DIR)/stage3.bin $(TOOLS_DIR)/mkimage.py
	$(PYTHON) $(TOOLS_DIR)/mkimage.py \
		--stage1 $(BUILD_DIR)/stage1.bin \
		--stage2 $(BUILD_DIR)/stage2.bin \
		--stage3 $(BUILD_DIR)/stage3.bin \
		--output $@ \
		--size-mb 64

$(BUILD_DIR)/boot-debug.img: $(BUILD_DIR)/stage1.bin $(BUILD_DIR)/stage2.bin $(BUILD_DIR)/stage3-debug.bin $(TOOLS_DIR)/mkimage.py
	$(PYTHON) $(TOOLS_DIR)/mkimage.py \
		--stage1 $(BUILD_DIR)/stage1.bin \
		--stage2 $(BUILD_DIR)/stage2.bin \
		--stage3 $(BUILD_DIR)/stage3-debug.bin \
		--output $@ \
		--size-mb 64

clean:
	rm -rf $(BUILD_DIR)

run: $(BUILD_DIR)/boot.img
	$(QEMU) -drive file=$(BUILD_DIR)/boot.img,format=raw,if=ide -serial stdio

run-debug: $(BUILD_DIR)/boot-debug.img
	$(QEMU) -drive file=$(BUILD_DIR)/boot-debug.img,format=raw,if=ide -serial stdio

run-xhci: $(BUILD_DIR)/boot.img
	$(QEMU) -drive file=$(BUILD_DIR)/boot.img,format=raw,if=ide -device qemu-xhci,id=xhci -serial stdio

.PHONY: all release prod debug clean run run-debug run-xhci

