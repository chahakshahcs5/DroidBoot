#include "printf.h"
#include "../debug/serial.h"
#include "../debug/vga.h"

static void putc_both(char c) {
    serial_putchar(c);
    vga_putchar(c);
}

static void puts_both(const char *s) {
    while (*s) {
        putc_both(*s++);
    }
}

static void print_hex(uint32_t val, int width, int pad_zero) {
    const char hex_chars[] = "0123456789ABCDEF";
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex_chars[val & 0xF];
        val >>= 4;
    }
    
    // Find first non-zero character
    int first_nonzero = 7;
    for (int i = 0; i < 8; i++) {
        if (buf[i] != '0') {
            first_nonzero = i;
            break;
        }
    }
    
    int digits_to_print = 8 - first_nonzero;
    if (digits_to_print < width) {
        digits_to_print = width;
    }
    if (digits_to_print > 8) digits_to_print = 8;
    if (digits_to_print == 0) digits_to_print = 1;

    int start_idx = 8 - digits_to_print;
    if (!pad_zero && width > 0) {
        // Space pad if not zero pad
        int spaces = width - (8 - first_nonzero);
        for (int s = 0; s < spaces; s++) putc_both(' ');
        start_idx = first_nonzero;
    }

    puts_both(&buf[start_idx]);
}

static void print_dec(int32_t val) {
    if (val < 0) {
        putc_both('-');
        val = -val;
    }
    char buf[12];
    int i = 0;
    if (val == 0) {
        putc_both('0');
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        putc_both(buf[j]);
    }
}

static void print_udec(uint32_t val) {
    char buf[12];
    int i = 0;
    if (val == 0) {
        putc_both('0');
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        putc_both(buf[j]);
    }
}

void vprintk(const char *fmt, va_list args) {
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            int pad_zero = 0;
            int width = 0;

            if (*fmt == '0') {
                pad_zero = 1;
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    puts_both(s);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    putc_both(c);
                    break;
                }
                case 'd': {
                    int32_t d = va_arg(args, int32_t);
                    print_dec(d);
                    break;
                }
                case 'u': {
                    uint32_t u = va_arg(args, uint32_t);
                    print_udec(u);
                    break;
                }
                case 'x':
                case 'X': {
                    uint32_t x = va_arg(args, uint32_t);
                    print_hex(x, width, pad_zero);
                    break;
                }
                case 'p': {
                    uint32_t p = va_arg(args, uint32_t);
                    puts_both("0x");
                    print_hex(p, 8, 1);
                    break;
                }
                case '%': {
                    putc_both('%');
                    break;
                }
                default: {
                    putc_both('%');
                    putc_both(*fmt);
                    break;
                }
            }
        } else {
            putc_both(*fmt);
        }
        fmt++;
    }
}

void printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
}

void log_info(const char *tag, const char *fmt, ...) {
    printk("[%s] ", tag);
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
    printk("\n");
}

void log_error(const char *tag, const char *fmt, ...) {
    printk("[%s ERROR] ", tag);
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
    printk("\n");
}
