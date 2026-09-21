#include "serial.h"
#include "../../include/io.h"

#define COM1_PORT 0x3F8

static bool serial_present = false;

void serial_init(void) {
    // 8250/16550 UART detection: write and read scratchpad register (port + 7)
    // On systems without a UART, reading an unpopulated I/O port returns 0xFF floating bus.
    outb(COM1_PORT + 7, 0x55);
    if (inb(COM1_PORT + 7) != 0x55) {
        serial_present = false;
        return;
    }
    outb(COM1_PORT + 7, 0xAA);
    if (inb(COM1_PORT + 7) != 0xAA) {
        serial_present = false;
        return;
    }
    serial_present = true;

    outb(COM1_PORT + 1, 0x00);    // Disable all interrupts
    outb(COM1_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(COM1_PORT + 0, 0x01);    // Set divisor to 1 (115200 baud)
    outb(COM1_PORT + 1, 0x00);    //                  (hi byte)
    outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

bool serial_is_present(void) {
    return serial_present;
}

static int is_transmit_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

void serial_putchar(char c) {
    if (!serial_present) return;
    if (c == '\n') {
        while (!is_transmit_empty());
        outb(COM1_PORT, '\r');
    }
    while (!is_transmit_empty());
    outb(COM1_PORT, (uint8_t)c);
}

void serial_puts(const char *s) {
    if (!serial_present) return;
    while (*s) {
        serial_putchar(*s++);
    }
}

