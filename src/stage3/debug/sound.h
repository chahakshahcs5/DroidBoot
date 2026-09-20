#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>
#include "../../include/io.h"

// PC Speaker audio feedback helper for hardware testing
static inline void sound_beep(uint16_t freq_hz, uint32_t ms) {
    if (freq_hz == 0) return;
    uint32_t div = 1193180 / freq_hz;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 3); // Enable speaker gate and timer 2

    for (uint32_t i = 0; i < ms * 1000; i++) {
        io_wait();
    }

    outb(0x61, tmp & ~3); // Disable speaker
}

// Ascending double-tone when Stage 3 begins
static inline void sound_boot_tone(void) {
    sound_beep(880, 80);
    for (int i = 0; i < 40000; i++) io_wait();
    sound_beep(1175, 120);
}

// Chime when Android phone is detected and MTP initialized
static inline void sound_phone_connected_tone(void) {
    sound_beep(1320, 80);
    for (int i = 0; i < 30000; i++) io_wait();
    sound_beep(1760, 150);
}

// Low buzz if an error occurs
static inline void sound_error_tone(void) {
    sound_beep(440, 200);
    for (int i = 0; i < 40000; i++) io_wait();
    sound_beep(330, 300);
}

// Fanfare when jumping into Linux kernel
static inline void sound_kernel_jump_tone(void) {
    sound_beep(1046, 60);
    for (int i = 0; i < 20000; i++) io_wait();
    sound_beep(1318, 60);
    for (int i = 0; i < 20000; i++) io_wait();
    sound_beep(1568, 120);
}

#endif // SOUND_H
