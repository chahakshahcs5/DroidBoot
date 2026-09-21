#ifndef BOOTMANAGER_RTC_H
#define BOOTMANAGER_RTC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../include/io.h"
#include "printf.h"

static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static inline uint8_t bcd_to_bin(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

static inline void rtc_get_timestamp_str(char *out, size_t max_len) {
    if (!out || max_len == 0) return;

    // Check if RTC is currently updating (bit 7 of register 0x0A)
    int timeout = 10000;
    outb(0x70, 0x0A);
    while ((inb(0x71) & 0x80) && --timeout > 0) io_wait();

    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t yr   = cmos_read(0x09);
    uint8_t regb = cmos_read(0x0B);

    // If BCD mode (bit 2 of reg B is 0)
    if (!(regb & 0x04)) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
    }
    // Handle 12-hour mode
    if (!(regb & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    uint32_t year = 2000 + yr;
    if (year < 2024 || year > 2099 || mon < 1 || mon > 12 || day < 1 || day > 31) {
        // Fallback if RTC uninitialized: use tick counter
        static uint32_t seq = 1;
        snprintf(out, max_len, "session_%u", seq++);
    } else {
        snprintf(out, max_len, "%04u%02u%02u_%02u%02u%02u", year, mon, day, hour, min, sec);
    }
}

#endif // BOOTMANAGER_RTC_H
