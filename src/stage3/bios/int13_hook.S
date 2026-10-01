# ==============================================================================
# INT 13h Hook — Real-Mode IVT Interceptor with PM Dispatch
#
# This code installs a custom INT 13h handler into the IVT (vector 0x13).
# When an INT 13h call targets our virtual drive, the handler:
#   1. Saves real-mode state (including SS:SP)
#   2. Switches to 32-bit Protected Mode
#   3. Calls int13_emu_dispatch() in C (which routes reads through xHCI MSC)
#   4. Switches back to Real Mode
#   5. Restores SS:SP, sets output registers, and returns with CF status
#
# For non-virtual drive numbers, transparently chains to the original BIOS handler.
#
# Memory Layout (all below 640KB in conventional memory):
#   0x5000 — int13_emu_data_t (resident data block, 32 bytes)
#   0x5080 — HOOK_MAILBOX (parameter passing between RM and PM, 32 bytes)
#   0x5200 — Hook blob code (copied here by int13_hook_install, ~400 bytes)
#   0x5F00 — Temporary PM stack (grows down, 512 bytes)
#   0x10000 — Bounce buffer (64KB for sector DMA)
# ==============================================================================

.code32
.section .text

# Fixed memory addresses
.equ EMU_DATA_ADDR,     0x5000    # int13_emu_data_t location
.equ HOOK_MAILBOX,      0x5080    # Mailbox for RM<->PM parameter passing
.equ HOOK_BASE,         0x5200    # Hook code address in low memory

# Offsets into int13_emu_data_t
.equ EMU_VDRIVE,        0         # uint8_t  virtual_drive
.equ EMU_ACTIVE,        1         # uint8_t  active
.equ EMU_ORIG_VEC,      2         # uint32_t original_int13_vec
.equ EMU_RES_CH,        27        # uint8_t  res_ch
.equ EMU_RES_CL,        28        # uint8_t  res_cl
.equ EMU_RES_DH,        29        # uint8_t  res_dh
.equ EMU_RES_DL,        30        # uint8_t  res_dl
.equ EMU_ORIG_SWAP_DRIVE, 31      # uint8_t  orig_swap_drive

# Mailbox layout (at HOOK_MAILBOX = 0x5080):
#   +0x00: AH function code (uint8_t)
#   +0x01: drive number DL (uint8_t)
#   +0x02: DAP physical address (uint32_t)
#   +0x06: return code from C handler (uint8_t)
#   +0x08: saved 32-bit PM ESP (uint32_t)
#   +0x0C: saved RM AX (uint16_t)
#   +0x0E: saved RM BX (uint16_t)
#   +0x10: saved RM CX (uint16_t)
#   +0x12: saved RM DX (uint16_t)
#   +0x14: saved RM SI (uint16_t)
#   +0x16: saved RM DI (uint16_t)
#   +0x18: saved RM DS (uint16_t)
#   +0x1A: saved RM ES (uint16_t)
#   +0x1C: saved RM SS (uint16_t)
#   +0x1E: saved RM SP (uint16_t)

# ==============================================================================
# void int13_hook_install(uint32_t emu_data_addr)
#
# Copies the hook stub to low memory (0x5200) and patches IVT[0x13].
# Called from 32-bit protected mode.
# Note: Stage 3 runs with CLI. Do NOT execute STI in protected mode!
# ==============================================================================
.globl int13_hook_install
int13_hook_install:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    pushl %ecx

    # Copy hook stub code to HOOK_BASE (0x5200)
    movl  $int13_hook_blob_start, %esi
    movl  $HOOK_BASE, %edi
    movl  $(int13_hook_blob_end - int13_hook_blob_start), %ecx
    cld
    rep movsb

    # Patch IVT[0x13] to point to our hook entry at 0000:HOOK_BASE
    # IVT entry format: offset (16-bit) | segment (16-bit) packed as uint32_t
    movl  $0x0000004C, %edi      # IVT[0x13] = 0x13 * 4 = 0x4C
    movl  $HOOK_BASE, %eax       # offset = HOOK_BASE, segment = 0x0000
    movl  %eax, (%edi)

    popl  %ecx
    popl  %edi
    popl  %esi
    popl  %ebp
    ret

# ==============================================================================
# void int13_hook_uninstall(void)
#
# Restores original IVT[0x13] vector. Called from 32-bit PM.
# ==============================================================================
.globl int13_hook_uninstall
int13_hook_uninstall:
    movl  (EMU_DATA_ADDR + EMU_ORIG_VEC), %eax
    movl  %eax, (0x0000004C)     # Restore IVT[0x13]
    ret

# ==============================================================================
# The Hook Blob — Copied to HOOK_BASE (0x5200) and executed in Real Mode
# ==============================================================================
int13_hook_blob_start:

.code16
int13_hook_entry:
    cmpb  $1, %cs:(EMU_DATA_ADDR + EMU_ACTIVE)
    jne   .chain_original

    # Check if DL is the swapped original drive (orig_swap_drive)
    movb  %cs:(EMU_DATA_ADDR + EMU_ORIG_SWAP_DRIVE), %al
    testb %al, %al
    jz    .check_virtual_drive
    cmpb  %al, %dl
    jne   .check_virtual_drive
    # Redirect swapped original drive back to physical drive 0x80
    movb  $0x80, %dl
    jmp   .chain_original

.check_virtual_drive:
    cmpb  %cs:(EMU_DATA_ADDR + EMU_VDRIVE), %dl
    jne   .chain_original

    # === OUR VIRTUAL DRIVE — Handle it ===
    # Save all caller registers to mailbox (using %cs: prefix)
    movb  %ah, %cs:(HOOK_MAILBOX + 0x00)
    movb  %dl, %cs:(HOOK_MAILBOX + 0x01)
    movw  %ax, %cs:(HOOK_MAILBOX + 0x0C)
    movw  %bx, %cs:(HOOK_MAILBOX + 0x0E)
    movw  %cx, %cs:(HOOK_MAILBOX + 0x10)
    movw  %dx, %cs:(HOOK_MAILBOX + 0x12)
    movw  %si, %cs:(HOOK_MAILBOX + 0x14)
    movw  %di, %cs:(HOOK_MAILBOX + 0x16)
    movw  %ds, %cs:(HOOK_MAILBOX + 0x18)
    movw  %es, %cs:(HOOK_MAILBOX + 0x1A)
    movw  %ss, %cs:(HOOK_MAILBOX + 0x1C)
    movw  %sp, %cs:(HOOK_MAILBOX + 0x1E)

    # Compute physical buffer address:
    # For AH=0x02 (legacy CHS read), buffer is at ES:BX
    # For all others (AH=0x42, AH=0x48, etc.), buffer/DAP is at DS:SI
    cmpb  $0x02, %ah
    je    .calc_es_bx
    movw  %ds, %ax
    movzwl %ax, %eax
    shll  $4, %eax
    movzwl %si, %esi
    addl  %esi, %eax
    jmp   .store_dap_phys

.calc_es_bx:
    movw  %es, %ax
    movzwl %ax, %eax
    shll  $4, %eax
    movzwl %bx, %ebx
    addl  %ebx, %eax

.store_dap_phys:
    movl  %eax, %cs:(HOOK_MAILBOX + 0x02)

    # Disable interrupts during switch to PM
    cli

    # Load 32-bit GDT from low memory copy
    lgdt  %cs:(HOOK_BASE + (int13_hook_gdt_desc_rm - int13_hook_blob_start))

    # Enable Protected Mode (CR0.PE = 1)
    movl  %cr0, %eax
    orl   $1, %eax
    movl  %eax, %cr0

    # Far jump to 32-bit PM trampoline
    .byte 0x66
    .byte 0xEA
    .long (HOOK_BASE + (int13_pm_trampoline - int13_hook_blob_start))
    .word 0x08                           # 32-bit code segment selector

.code32
int13_pm_trampoline:
    # Set up 32-bit data segments
    movw  $0x10, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss
    movw  %ax, %fs
    movw  %ax, %gs

    # Switch to private 32-bit stack at 0x5F00
    movl  $0x5F00, %esp

    # Call C handler: int13_emu_dispatch(ah_function, dap_phys, drive)
    movzbl (HOOK_MAILBOX + 0x01), %eax  # drive
    pushl  %eax
    movl   (HOOK_MAILBOX + 0x02), %eax  # dap_phys
    pushl  %eax
    movzbl (HOOK_MAILBOX + 0x00), %eax  # ah_function
    pushl  %eax

    call   int13_emu_dispatch
    movb   %al, (HOOK_MAILBOX + 0x06)   # Store return code
    addl   $12, %esp

    # Return to 16-bit Protected Mode
    ljmp   $0x18, $(HOOK_BASE + (int13_rm_return - int13_hook_blob_start))

.code16
int13_rm_return:
    # 16-bit data segment
    movw  $0x20, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss

    # Clear PE bit (CR0.PE = 0)
    movl  %cr0, %eax
    andl  $0xFFFFFFFE, %eax
    movl  %eax, %cr0

    # Far jump to Real Mode (CS = 0x0000)
    ljmp  $0x0000, $(HOOK_BASE + (int13_rm_real - int13_hook_blob_start))

int13_rm_real:
    # Back in Real Mode!
    # Load Real Mode IVT (Limit 0x3FF, Base 0)
    lidt  %cs:(HOOK_BASE + (int13_rm_idt - int13_hook_blob_start))

    # Restore caller's Real Mode SS and SP
    movw  %cs:(HOOK_MAILBOX + 0x1C), %ax
    movw  %ax, %ss
    movw  %cs:(HOOK_MAILBOX + 0x1E), %sp

    # Restore caller's segments and general registers
    movw  %cs:(HOOK_MAILBOX + 0x1A), %es
    movw  %cs:(HOOK_MAILBOX + 0x18), %ds
    movw  %cs:(HOOK_MAILBOX + 0x16), %di
    movw  %cs:(HOOK_MAILBOX + 0x14), %si
    movw  %cs:(HOOK_MAILBOX + 0x12), %dx
    movw  %cs:(HOOK_MAILBOX + 0x10), %cx
    movw  %cs:(HOOK_MAILBOX + 0x0E), %bx

    # Restore AL from caller's input (needed by AH=0x02 to return sector count in AL)
    movb  %cs:(HOOK_MAILBOX + 0x0C), %al

    # Check return code
    movb  %cs:(HOOK_MAILBOX + 0x06), %ah
    testb %ah, %ah
    jnz   .handler_error

    # === SUCCESS ===
    cmpb  $0x41, %cs:(HOOK_MAILBOX + 0x00)
    je    .ret_41h
    cmpb  $0x08, %cs:(HOOK_MAILBOX + 0x00)
    je    .ret_08h
    cmpb  $0x15, %cs:(HOOK_MAILBOX + 0x00)
    je    .ret_15h

    # Default success (AH=00h, 01h, 02h, 04h, 42h, 48h, etc.): AH=0, CF=0
    xorb  %ah, %ah
    clc
    lretw $2

.ret_41h:
    # AH=41h return: BX=0xAA55, CX=1, AH=0x21, CF=0
    movw  $0xAA55, %bx
    movw  $0x0001, %cx
    movb  $0x21, %ah
    clc
    lretw $2

.ret_08h:
    # AH=08h return: CH, CL, DH, DL from emu_data
    movb  %cs:(EMU_DATA_ADDR + EMU_RES_CH), %ch
    movb  %cs:(EMU_DATA_ADDR + EMU_RES_CL), %cl
    movb  %cs:(EMU_DATA_ADDR + EMU_RES_DH), %dh
    movb  %cs:(EMU_DATA_ADDR + EMU_RES_DL), %dl
    xorb  %ah, %ah
    clc
    lretw $2

.ret_15h:
    # AH=15h return: AH=0x03 (fixed disk), CF=0
    movb  $0x03, %ah
    clc
    lretw $2

.handler_error:
    # Error return: AH = error code, CF = 1
    stc
    lretw $2

.chain_original:
    # Not our drive — chain to original BIOS INT 13h handler
    # Stack has: FLAGS, CS, IP from the caller's INT instruction.
    # Push original segment and offset, then lretw jumps to it without disturbing stack.
    pushw %cs:(EMU_DATA_ADDR + EMU_ORIG_VEC + 2)   # Original handler segment
    pushw %cs:(EMU_DATA_ADDR + EMU_ORIG_VEC)       # Original handler offset
    lretw

# --- Data tables inside hook blob ---
.align 4
int13_rm_idt:
    .word 0x03FF                # Limit: 1023
    .long 0x00000000            # Base: 0x00000000

.align 8
int13_hook_gdt_start:
    .quad 0x0000000000000000    # 0x00: Null Descriptor
    .quad 0x00CF9A000000FFFF    # 0x08: 32-bit Code (Base 0, Limit 4GB, DPL 0)
    .quad 0x00CF92000000FFFF    # 0x10: 32-bit Data (Base 0, Limit 4GB, DPL 0)
    .quad 0x00009A000000FFFF    # 0x18: 16-bit Code (Base 0, Limit 64KB, DPL 0)
    .quad 0x000092000000FFFF    # 0x20: 16-bit Data (Base 0, Limit 64KB, DPL 0)
int13_hook_gdt_end:

.align 4
int13_hook_gdt_desc_rm:
    .word (int13_hook_gdt_end - int13_hook_gdt_start - 1)
    .long (HOOK_BASE + (int13_hook_gdt_start - int13_hook_blob_start))

int13_hook_blob_end:

.section .note.GNU-stack,"",@progbits
