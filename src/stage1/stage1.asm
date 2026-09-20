; ==============================================================================
; Stage 1: Legacy BIOS MBR Boot Sector (512 Bytes)
; Target: x86 Real Mode (16-bit)
; Load Address: 0x0000:0x7C00
; Responsibilities:
;   - Establish canonical segment registers & stack
;   - Preserve BIOS boot drive from DL
;   - Verify INT 13h BIOS Extensions (LBA)
;   - Load Stage 2 into 0x0000:0x8000 via INT 13h AH=0x42 (Extended Read)
;   - Verify Stage 2 magic signature ("STG2")
;   - Pass boot drive in DL and jump to Stage 2
; ==============================================================================

[BITS 16]
[ORG 0x7C00]

entry:
    jmp 0x0000:.canonicalize        ; Normalize CS:IP to 0x0000:0x7C00

.canonicalize:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00                  ; Stack grows downwards from 0x7C00
    cld
    sti

    ; Preserve BIOS boot drive passed in DL
    mov [boot_drive], dl

    ; Print early banner
    mov si, msg_stage1
    call print_str

    ; Reset disk system (INT 13h AH=0x00)
    xor ax, ax
    mov dl, [boot_drive]
    int 0x13
    jc .disk_error

    ; Check for INT 13h Extensions (AH=0x41, BX=0x55AA)
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no_lba_support
    cmp bx, 0xAA55
    jne .no_lba_support

    ; Populate DAP with patched Stage 2 values
    mov eax, [stage2_lba]
    mov [dap_lba_low], eax
    mov ax, [stage2_sec]
    mov [dap_sectors], ax

    ; Read Stage 2 into 0x0000:0x8000 via INT 13h AH=0x42
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .read_failed

    ; Verify Stage 2 magic ("STG2" = 0x32475453) at 0x8004
    cmp dword [0x8004], 0x32475453
    jne .invalid_magic

    ; Stage 2 loaded successfully
    mov si, msg_ok
    call print_str

    ; Pass boot drive in DL and transfer execution to Stage 2
    mov dl, [boot_drive]
    jmp 0x0000:0x8000

.disk_error:
    mov si, msg_disk_err
    call print_str
    jmp .halt

.no_lba_support:
    mov si, msg_no_lba
    call print_str
    jmp .halt

.read_failed:
    mov si, msg_read_err
    call print_str
    jmp .halt

.invalid_magic:
    mov si, msg_bad_magic
    call print_str
    jmp .halt

.halt:
    cli
    hlt
    jmp .halt

; ------------------------------------------------------------------------------
; Helper: print_str (prints null-terminated string at DS:SI via INT 10h AH=0x0E)
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
; Data Section
; ------------------------------------------------------------------------------
boot_drive:     db 0

msg_stage1:     db 13, 10, "[STAGE1] BIOS Boot Drive: OK, Loading Stage 2... ", 0
msg_ok:         db "OK", 13, 10, 0
msg_disk_err:   db 13, 10, "[STAGE1] Disk reset error!", 13, 10, 0
msg_no_lba:     db 13, 10, "[STAGE1] BIOS INT 13h LBA extensions not supported!", 13, 10, 0
msg_read_err:   db 13, 10, "[STAGE1] Read Stage 2 failed!", 13, 10, 0
msg_bad_magic:  db 13, 10, "[STAGE1] Corrupted Stage 2 signature!", 13, 10, 0

; ------------------------------------------------------------------------------
; Disk Address Packet (DAP) for INT 13h AH=0x42
; ------------------------------------------------------------------------------
align 4
dap:
    db 0x10                         ; Size of DAP (16 bytes)
    db 0x00                         ; Reserved (always 0)
dap_sectors:
    dw 8                            ; Number of sectors to read
dap_buf_offset:
    dw 0x8000                       ; Target offset: 0x8000
dap_buf_segment:
    dw 0x0000                       ; Target segment: 0x0000
dap_lba_low:
    dd 1                            ; Starting LBA (low 32 bits)
dap_lba_high:
    dd 0                            ; Starting LBA (high 32 bits)

; ------------------------------------------------------------------------------
; Patch Table (at fixed offset 0x1B0 = 432 bytes)
; mkimage.py populates these values
; ------------------------------------------------------------------------------
times 432 - ($ - $$) db 0
stage2_lba:     dd 1                ; Patched by mkimage.py (LBA 1)
stage2_sec:     dw 8                ; Patched by mkimage.py (8 sectors)

; Pad to 510 bytes (leaving 64 bytes for MBR partition table if needed)
times 510 - ($ - $$) db 0
dw 0xAA55                           ; Boot sector signature
