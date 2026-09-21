#include "timer.h"
#include "../../include/io.h"
#include "printf.h"

static uint32_t tsc_per_us = 1000; // Default: 1 GHz until calibrated
static uint64_t boot_tsc = 0;
static bool     timer_calibrated = false;

// Freestanding 64-bit by 32-bit hardware division using x86 divl instruction
static inline uint64_t udiv64_32(uint64_t n, uint32_t d) {
    if (d == 0) return 0;
    uint32_t n_hi = (uint32_t)(n >> 32);
    uint32_t n_lo = (uint32_t)n;
    uint32_t q_hi = 0, q_lo = 0, rem = 0;
    __asm__ volatile ("divl %4" : "=a"(q_hi), "=d"(rem) : "a"(n_hi), "d"(0), "r"(d));
    __asm__ volatile ("divl %4" : "=a"(q_lo), "=d"(rem) : "a"(n_lo), "d"(rem), "r"(d));
    return ((uint64_t)q_hi << 32) | q_lo;
}

// Software 64-bit unsigned division intrinsics for 32-bit GCC without libgcc
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rp) {
    if (d == 0) {
        if (rp) *rp = 0;
        return 0;
    }
    uint64_t q = 0;
    uint64_t r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }
    if (rp) *rp = r;
    return q;
}

uint64_t __udivdi3(uint64_t a, uint64_t b) {
    return __udivmoddi4(a, b, NULL);
}

uint64_t __umoddi3(uint64_t a, uint64_t b) {
    uint64_t rem = 0;
    __udivmoddi4(a, b, &rem);
    return rem;
}

uint64_t timer_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void timer_init(void) {
    boot_tsc = timer_rdtsc();

    // Calibrate TSC using PIT Channel 2 one-shot mode (10 ms countdown)
    uint8_t orig_61 = inb(0x61);

    // Set PIT Channel 2 to Mode 0 (Interrupt on Terminal Count), LSB/MSB
    outb(0x43, 0xB0);

    // 10 ms of PIT 1,193,182 Hz clock = 11,932 counts
    const uint16_t countdown = 11932;
    outb(0x42, (uint8_t)(countdown & 0xFF));
    outb(0x42, (uint8_t)((countdown >> 8) & 0xFF));

    // Reset gate low then high to start counter
    outb(0x61, orig_61 & ~0x01);
    uint64_t start_tsc = timer_rdtsc();
    outb(0x61, (orig_61 & ~0x02) | 0x01);

    // Wait until Channel 2 OUT (bit 5 of port 0x61) goes high (countdown expired)
    int timeout = 100000;
    while (!(inb(0x61) & 0x20) && --timeout > 0) {
        __asm__ volatile ("pause");
    }

    uint64_t end_tsc = timer_rdtsc();

    // Restore original port 0x61
    outb(0x61, orig_61);

    if (timeout > 0 && end_tsc > start_tsc) {
        uint64_t diff = end_tsc - start_tsc;
        // 10,000 microseconds in 10 ms
        tsc_per_us = (uint32_t)udiv64_32(diff, 10000);
        if (tsc_per_us == 0) tsc_per_us = 1000;
        timer_calibrated = true;
        log_info("TIMER", "Calibrated CPU TSC: %u MHz (%u ticks/us)", tsc_per_us, tsc_per_us);
    } else {
        tsc_per_us = 2000; // Fallback to 2.0 GHz
        log_info("TIMER", "PIT calibration timed out, using fallback 2.0 GHz clock.");
    }
}

void timer_udelay(uint32_t us) {
    if (!timer_calibrated && tsc_per_us == 0) tsc_per_us = 1000;
    uint64_t ticks = (uint64_t)us * tsc_per_us;
    uint64_t start = timer_rdtsc();
    while ((timer_rdtsc() - start) < ticks) {
        __asm__ volatile ("pause");
    }
}

void timer_mdelay(uint32_t ms) {
    while (ms > 0) {
        uint32_t chunk = ms > 100 ? 100 : ms;
        timer_udelay(chunk * 1000);
        ms -= chunk;
    }
}

uint32_t timer_get_ms(void) {
    uint64_t cur = timer_rdtsc();
    if (cur < boot_tsc) return 0;
    uint64_t elapsed_ticks = cur - boot_tsc;
    uint32_t ticks_per_ms = tsc_per_us * 1000;
    if (ticks_per_ms == 0) ticks_per_ms = 1000000;
    return (uint32_t)udiv64_32(elapsed_ticks, ticks_per_ms);
}
