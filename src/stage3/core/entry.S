.code32
.section .text.entry
.globl stage3_entry
.extern c_main
.extern __bss_start
.extern __bss_end

stage3_entry:
    cld

    # Clear direction flag and align stack
    and $0xFFFFFFF0, %esp

    # Pass boot_info_t pointer from EAX
    push %eax

    # Zero BSS section
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    # Jump to C runtime entry
    call c_main

.hang:
    cli
    hlt
    jmp .hang

.section .note.GNU-stack,"",@progbits
