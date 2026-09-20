; ==============================================================================
; Stage 2: Bootstrap Loader & 32-bit Protected Mode Handoff
; Target: x86 Real Mode (16-bit) -> 32-bit Protected Mode
; Load Address: 0x0000:0x8000
; Responsibilities:
;   - Expose fixed header at offset 0 (Jump, Magic, Stage 3 LBA, Sectors, Addr)
;   - Probe E820 system memory map (INT 15h, AX=0xE820) into 0x9000
;   - Enable A20 address line
;   - Load Stage 3 from disk sectors into low RAM (0x1000:0x0000 = 0x10000) via INT 13h
;   - Establish 32-bit Global Descriptor Table (GDT)
;   - Switch CPU to 32-bit Protected Mode (CR0.PE = 1)
;   - In 32-bit Protected Mode, relocate Stage 3 from 0x10000 to 0x00100000 (1 MiB)
;   - Jump to Stage 3 entry point at 0x00100000
; ==============================================================================

[BITS 16]
[ORG 0x8000]

; ------------------------------------------------------------------------------
; Stage 2 Header (Offset 0x0000 - 0x001F)
; ------------------------------------------------------------------------------
stage2_header:
    jmp short entry
    nop
    nop
    magic:              dd 0x32475453       ; "STG2" (Offset 0x04)
    stage3_start_lba:   dd 9                ; Patched by mkimage.py (Offset 0x08)
    stage3_sectors:     dd 64               ; Patched by mkimage.py (Offset 0x0C)
    stage3_load_addr:   dd 0x00100000       ; Patched by mkimage.py (Offset 0x10)

; ------------------------------------------------------------------------------
; Entry Point
; ------------------------------------------------------------------------------
align 4
entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                          ; Use stack below 0x7C00
    cld
    sti

    mov [boot_drive], dl

    mov si, msg_stage2
    call print_str

    ; 1. Query E820 Memory Map
    mov si, msg_e820
    call print_str
    call probe_e820
    mov si, msg_ok
    call print_str

    ; 2. Enable A20 Gate
    mov si, msg_a20
    call print_str
    call enable_a20
    mov si, msg_ok
    call print_str

    ; 3. Load Stage 3 into low memory buffer (starting at 0x1000:0x0000 = physical 0x10000)
    mov si, msg_loading_stage3
    call print_str

    mov eax, [stage3_start_lba]
    mov [stage3_dap_lba], eax
    mov cx, [stage3_sectors]                ; Remaining sectors to read
    mov word [stage3_dap_segment], 0x1000   ; Initial buffer segment 0x1000
    mov word [stage3_dap_offset], 0x0000

.read_stage3_loop:
    test cx, cx
    jz .read_stage3_done

    mov ax, cx
    cmp ax, 64                              ; Read max 64 sectors (32KB) per call to prevent 64KB boundary wrap
    jbe .do_chunk
    mov ax, 64

.do_chunk:
    mov [stage3_dap_count], ax
    push cx
    push ax

    mov si, stage3_dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .stage3_read_error

    pop ax                                  ; Sectors read in this chunk
    pop cx                                  ; Total remaining sectors

    sub cx, ax                              ; Decrement remaining sectors
    movzx edx, ax
    add [stage3_dap_lba], edx               ; Advance LBA

    ; Advance segment by (ax * 512) >> 4 = ax * 32 = ax << 5
    shl ax, 5
    add [stage3_dap_segment], ax

    jmp .read_stage3_loop

.read_stage3_done:
    mov si, msg_ok
    call print_str

    ; 4. Transition to 32-bit Flat Protected Mode
    mov si, msg_entering_pmode
    call print_str

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1                               ; Set PE (Protection Enable) bit
    mov cr0, eax

    ; Far jump to flush CPU pipeline and set CS to 32-bit code selector (0x08)
    jmp 0x08:pmode_entry

.stage3_read_error:
    mov si, msg_stage3_err
    call print_str
    cli
    hlt
    jmp $

; ------------------------------------------------------------------------------
; 32-bit Protected Mode Entry
; ------------------------------------------------------------------------------
[BITS 32]
pmode_entry:
    ; Reload segment registers with 32-bit Data Selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 32-bit stack (0x0009FFF0, grows downwards to 0x00080000)
    mov esp, 0x0009FFF0

    ; In Protected Mode, relocate Stage 3 from low memory (0x00010000)
    ; to high memory load address (0x00100000) using 32-bit flat addressing
    mov esi, 0x00010000                     ; Source buffer
    mov edi, [0x8010]                       ; Destination address (stage3_load_addr = 0x00100000)
    movzx ecx, word [0x800C]                ; stage3_sectors
    shl ecx, 7                              ; 512 bytes / 4 = 128 dwords per sector
    cld
    rep movsd                               ; Fast 32-bit block move

    ; Pass boot parameter struct pointer in EAX
    ; Struct boot_info at 0x00008FF0:
    ;   uint8_t  boot_drive
    ;   uint8_t  reserved[3]
    ;   uint32_t e820_count
    ;   uint32_t e820_map_addr (0x00009000)
    mov dword [0x8FF0], 0
    mov al, [boot_drive]
    mov byte [0x8FF0], al
    mov ecx, [e820_entry_count]
    mov [0x8FF4], ecx
    mov dword [0x8FF8], 0x00009000

    mov eax, 0x00008FF0                     ; Argument: pointer to boot_info

    ; Jump to Stage 3 entry point at 0x00100000
    mov edx, [0x8010]
    jmp edx

; ------------------------------------------------------------------------------
; 16-bit Subroutines
; ------------------------------------------------------------------------------
[BITS 16]

; ------------------------------------------------------------------------------
; Subroutine: probe_e820
; Queries INT 15h, AX=0xE820 into 0x0000:0x9000
; ------------------------------------------------------------------------------
probe_e820:
    pusha
    xor ebx, ebx
    xor bp, bp                              ; Entry counter
    mov di, 0x9000                          ; Destination buffer

.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150                     ; "SMAP"
    mov ecx, 24                             ; Request 24 bytes per entry
    mov [di + 20], dword 1                  ; Force ACPI 3.0 attribute to 1
    int 0x15
    jc .e820_done                           ; CF set = done or not supported
    cmp eax, 0x534D4150
    jne .e820_failed

    inc bp
    add di, 24
    test ebx, ebx
    jz .e820_done
    cmp bp, 128                             ; Safety limit: max 128 entries
    jae .e820_done
    jmp .e820_loop

.e820_done:
    mov [e820_entry_count], ebp
    popa
    ret

.e820_failed:
    mov si, msg_e820_err
    call print_str
    cli
    hlt
    jmp $

; ------------------------------------------------------------------------------
; Subroutine: enable_a20
; ------------------------------------------------------------------------------
enable_a20:
    pusha
    ; Fast A20 test & set via port 0x92
    in al, 0x92
    test al, 2
    jnz .a20_test
    or al, 2
    and al, 0xFE
    out 0x92, al

.a20_test:
    call check_a20
    test ax, ax
    jnz .a20_success

    ; Fallback: Keyboard Controller (8042)
    call empty_8042
    mov al, 0xD1
    out 0x64, al
    call empty_8042
    mov al, 0xDF
    out 0x60, al
    call empty_8042

    call check_a20
    test ax, ax
    jnz .a20_success

    mov si, msg_a20_err
    call print_str
    cli
    hlt
    jmp $

.a20_success:
    popa
    ret

empty_8042:
    in al, 0x64
    test al, 2
    jnz empty_8042
    ret

; Returns AX = 1 if A20 enabled, 0 if disabled
check_a20:
    pushf
    push ds
    push es
    push di
    push si

    xor ax, ax
    mov es, ax
    not ax
    mov ds, ax                              ; DS = 0xFFFF

    mov di, 0x7DFE
    mov si, 0x7E0E                          ; 0xFFFF:0x7E0E wraps to 0x0000:0x7DFE if A20 disabled

    mov al, [es:di]
    push ax
    mov al, [ds:si]
    push ax

    mov byte [es:di], 0x00
    mov byte [ds:si], 0xFF
    cmp byte [es:di], 0xFF

    pop ax
    mov [ds:si], al
    pop ax
    mov [es:di], al

    mov ax, 0
    je .check_done                          ; If equal, A20 is disabled
    mov ax, 1

.check_done:
    pop si
    pop di
    pop es
    pop ds
    popf
    ret

; ------------------------------------------------------------------------------
; Helper: print_str
; ------------------------------------------------------------------------------
print_str:
    pusha
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    jmp .loop
.done:
    popa
    ret

; ------------------------------------------------------------------------------
; Data & Structures
; ------------------------------------------------------------------------------
boot_drive:         db 0
e820_entry_count:   dd 0

msg_stage2:         db "[STAGE2] Bootstrap alive at 0x8000", 13, 10, 0
msg_e820:           db "[STAGE2] Probing E820 system memory map... ", 0
msg_a20:            db "[STAGE2] Enabling A20 gate... ", 0
msg_loading_stage3: db "[STAGE2] Loading Stage 3 C runtime... ", 0
msg_entering_pmode: db "[STAGE2] Switching CPU to 32-bit Flat Protected Mode...", 13, 10, 0
msg_ok:             db "OK", 13, 10, 0
msg_e820_err:       db 13, 10, "[STAGE2] E820 memory probe failed!", 13, 10, 0
msg_a20_err:        db 13, 10, "[STAGE2] Failed to enable A20 address line!", 13, 10, 0
msg_stage3_err:     db 13, 10, "[STAGE2] Failed to read Stage 3 sectors from disk!", 13, 10, 0

align 4
stage3_dap:
    db 0x10                                 ; DAP size (16 bytes)
    db 0x00                                 ; Reserved
stage3_dap_count:
    dw 64                                   ; Sectors to read
stage3_dap_offset:
    dw 0x0000                               ; Buffer offset: 0x0000
stage3_dap_segment:
    dw 0x1000                               ; Buffer segment: 0x1000 (Physical 0x10000)
stage3_dap_lba:
    dd 0                                    ; LBA low
    dd 0                                    ; LBA high

; ------------------------------------------------------------------------------
; Global Descriptor Table (GDT)
; ------------------------------------------------------------------------------
align 16
gdt_start:
    ; Null Descriptor (Selector 0x00)
    dd 0x00000000
    dd 0x00000000

    ; 32-bit Code Descriptor (Selector 0x08)
    ; Base: 0x00000000, Limit: 0xFFFFF (4 GiB with 4K granularity)
    ; Access: 0x9A (Present, Ring 0, Executable, Readable)
    ; Flags: 0xCF (4KB granularity, 32-bit operation)
    dw 0xFFFF                               ; Limit 15:00
    dw 0x0000                               ; Base 15:00
    db 0x00                                 ; Base 23:16
    db 0x9A                                 ; Access
    db 0xCF                                 ; Flags & Limit 19:16
    db 0x00                                 ; Base 31:24

    ; 32-bit Data Descriptor (Selector 0x10)
    ; Base: 0x00000000, Limit: 0xFFFFF (4 GiB)
    ; Access: 0x92 (Present, Ring 0, Data, Writable)
    ; Flags: 0xCF
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
gdt_end:

align 4
gdt_descriptor:
    dw gdt_end - gdt_start - 1              ; GDT limit
    dd gdt_start                            ; GDT physical base address
