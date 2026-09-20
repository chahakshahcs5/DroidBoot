.code32
.section .text
.globl bios_int13_call
.globl bios_int10_call

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
    # 0x7105: op_type (0 = INT 13h, 1 = INT 10h)
    # 0x7106: return code (2 bytes)
    # 0x7108: saved 32-bit ESP (4 bytes)
    movb %al, (0x7100)
    movw %dx, (0x7102)
    movb %cl, (0x7104)
    movb $0, (0x7105)       # op_type = 0 (INT 13h)
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

# ==============================================================================
# uint16_t bios_int10_call(uint16_t ax, uint16_t bx, uint16_t cx, uint16_t dx,
#                          uint16_t es, uint16_t di)
#
# Arguments:
#   8(%ebp)  = uint16_t ax
#  12(%ebp)  = uint16_t bx
#  16(%ebp)  = uint16_t cx
#  20(%ebp)  = uint16_t dx
#  24(%ebp)  = uint16_t es
#  28(%ebp)  = uint16_t di
#
# Returns:
#   AX return code from INT 10h (0x004F = VBE Success)
# ==============================================================================
bios_int10_call:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    movw 8(%ebp), %ax
    movw 12(%ebp), %bx
    movw 16(%ebp), %cx
    movw 20(%ebp), %dx
    movw 24(%ebp), %si      # ES
    movw 28(%ebp), %di      # DI

    # Mailbox for INT 10h:
    # 0x7105: op_type = 1 (INT 10h)
    # 0x7108: saved 32-bit ESP
    # 0x7110: AX
    # 0x7112: BX
    # 0x7114: CX
    # 0x7116: DX
    # 0x7118: ES
    # 0x711A: DI
    # 0x711C: ret_AX
    # 0x711E: ret_FLAGS
    movb $1, (0x7105)
    movl %esp, (0x7108)
    movw %ax, (0x7110)
    movw %bx, (0x7112)
    movw %cx, (0x7114)
    movw %dx, (0x7116)
    movw %si, (0x7118)
    movw %di, (0x711A)

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

    # Check op_type
    cmpb $1, (0x7105)
    je .pmode_ret_int10

    # Read INT 13h return code from mailbox
    movzwl (0x7106), %eax
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

.pmode_ret_int10:
    # Read INT 10h return code (AX) from mailbox
    movzwl (0x711C), %eax
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

    # Check op_type (0 = INT 13h, 1 = INT 10h)
    cmpb $1, (0x7105)
    je .rm_int10

    # --- INT 13h Disk Service ---
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
    jmp .switch_back

.rm_int10:
    # --- INT 10h Video / VBE Service ---
    movw (0x7118), %es      # ES
    movw (0x711A), %di      # DI
    movw (0x7110), %ax      # AX
    movw (0x7112), %bx      # BX
    movw (0x7114), %cx      # CX
    movw (0x7116), %dx      # DX

    clc
    sti
    int $0x10
    cli
    movw %ax, (0x711C)      # Save return AX
    jmp .switch_back

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
