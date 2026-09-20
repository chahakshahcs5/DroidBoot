.code32
.section .text
.globl bios_int13_call

# ==============================================================================
# int bios_int13_call(uint8_t drive, uint32_t dap_phys, uint8_t cmd)
#
# Arguments:
#   8(%ebp)  = uint8_t  drive (e.g. 0x80)
#  12(%ebp)  = uint32_t dap_phys (physical address of DAP, e.g. 0x7E00)
#  16(%ebp)  = uint8_t  cmd (0x42 = Extended Read, 0x43 = Extended Write)
#
# Returns:
#   0 on success, or (AH << 8) | CF on failure
# ==============================================================================
bios_int13_call:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    # Save arguments
    movb 8(%ebp), %al       # drive
    movl 12(%ebp), %edx     # dap_phys
    movb 16(%ebp), %cl      # cmd

    # Mailbox at 0x7100:
    # 0x7100: drive (1 byte)
    # 0x7102: dap offset (2 bytes, EDX & 0xFFFF)
    # 0x7104: cmd (1 byte)
    # 0x7106: return code (2 bytes)
    # 0x7108: saved 32-bit ESP (4 bytes)
    movb %al, (0x7100)
    movw %dx, (0x7102)
    movb %cl, (0x7104)
    movl %esp, (0x7108)

    # Copy thunk code and data table to 0x7000
    movl $thunk_blob_start, %esi
    movl $0x7000, %edi
    movl $(thunk_blob_end - thunk_blob_start), %ecx
    cld
    rep movsb

    # Load 16-bit GDT from low-memory copy
    lgdt (0x7000 + (thunk_gdt_desc - thunk_blob_start))

    # Far jump to 16-bit protected mode code selector (0x18) at 0x7000
    ljmp $0x18, $0x7000

# ------------------------------------------------------------------------------
# 32-bit entry point after returning from real mode
# ------------------------------------------------------------------------------
.globl pmode_return
pmode_return:
    # Reload 32-bit data segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Restore 32-bit ESP
    movl (0x7108), %esp

    # Read return code from mailbox
    movzwl (0x7106), %eax

    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

# ------------------------------------------------------------------------------
# The Thunk Blob (copied to 0x7000 and executed)
# ------------------------------------------------------------------------------
thunk_blob_start:

# 16-bit code starts at offset 0 (0x7000)
.code16
thunk_pmode16:
    # Set up 16-bit data segments
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    # Disable Protected Mode (CR0.PE = 0)
    movl %cr0, %eax
    andl $0xFFFFFFFE, %eax
    movl %eax, %cr0

    # Far jump to 16-bit Real Mode (CS = 0x0000, IP = 0x7000 + offset)
    ljmp $0x0000, $(0x7000 + (thunk_rm_entry - thunk_blob_start))

thunk_rm_entry:
    # In Real Mode!
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movw $0x7000, %sp       # Real-mode stack grows down from 0x7000

    # Load Real Mode IVT (Limit 0x3FF, Base 0)
    lidt (0x7000 + (thunk_rm_idt - thunk_blob_start))

    # Read mailbox parameters
    movb (0x7100), %dl      # drive
    movw (0x7102), %si      # DS:SI = DAP pointer
    movb (0x7104), %ah      # cmd (0x42 / 0x43)
    movb $0x00, %al         # verify = 0

    clc
    sti                     # Enable interrupts for BIOS disk controller handlers & timer ticks
    int $0x13
    cli                     # Disable interrupts before returning to 32-bit PM
    jc .disk_fail

    # Success: store 0
    movw $0x0000, (0x7106)
    jmp .switch_back

.disk_fail:
    testb %ah, %ah
    jnz .save_code
    movb $0xFF, %ah
.save_code:
    movw %ax, (0x7106)

.switch_back:
    cli

    # Reload GDT from low memory
    lgdt (0x7000 + (thunk_gdt_desc - thunk_blob_start))

    # Enable Protected Mode (CR0.PE = 1)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    # Far jump to 32-bit entry inside low memory (offset < 64KB)
    ljmpl $0x08, $(0x7000 + (thunk_pmode32 - thunk_blob_start))

.code32
thunk_pmode32:
    # Now in 32-bit Protected Mode!
    # Long jump back to high memory Stage 3 code
    movl $pmode_return, %eax
    jmpl *%eax

# ------------------------------------------------------------------------------
# Data tables inside thunk blob (all offsets < 64KB, copied to 0x7000)
# ------------------------------------------------------------------------------
.align 8
thunk_rm_idt:
    .word 0x03FF            # Limit: 1023
    .long 0x00000000        # Base: 0x00000000

.align 8
thunk_gdt_start:
    # 0x00: Null Descriptor
    .quad 0x0000000000000000
    # 0x08: 32-bit Code (Base 0, Limit 4GB, Type 0x9A, Flags 0xCF)
    .quad 0x00CF9A000000FFFF
    # 0x10: 32-bit Data (Base 0, Limit 4GB, Type 0x92, Flags 0xCF)
    .quad 0x00CF92000000FFFF
    # 0x18: 16-bit Code (Base 0, Limit 64KB, Type 0x9A, Flags 0x00)
    .quad 0x00009A000000FFFF
    # 0x20: 16-bit Data (Base 0, Limit 64KB, Type 0x92, Flags 0x00)
    .quad 0x000092000000FFFF
thunk_gdt_end:

.align 4
thunk_gdt_desc:
    .word (thunk_gdt_end - thunk_gdt_start - 1)
    .long (0x7000 + (thunk_gdt_start - thunk_blob_start))

thunk_blob_end:

.section .note.GNU-stack,"",@progbits
