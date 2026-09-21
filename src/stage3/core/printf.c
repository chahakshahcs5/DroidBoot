#include "printf.h"
#include "../debug/serial.h"
#include "../debug/vga.h"
#include "../debug/disk_log.h"

static uint32_t newline_counter = 0;

static void putc_both(char c) {
    serial_putchar(c);
    vga_putchar(c);
    disk_log_putc(c);
    if (c == '\n') {
        newline_counter++;
        // Every 20 newlines, auto-flush log to disk so printk output is captured
        if (newline_counter >= 20) {
            newline_counter = 0;
            disk_log_auto_flush_if_needed();
        }
    }
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
    disk_log_flush();
}

void log_error(const char *tag, const char *fmt, ...) {
    printk("[%s ERROR] ", tag);
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
    printk("\n");
    disk_log_flush();
}

#if IS_DEBUG_BUILD
void log_debug(const char *tag, const char *fmt, ...) {
    printk("[%s DEBUG] ", tag);
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
    printk("\n");
}
#endif

int vsnprintf(char *str, size_t size, const char *fmt, va_list args) {
    if (!str || size == 0) return 0;

    size_t out_idx = 0;
    #define EMIT_CHAR(ch) do { \
        if (out_idx + 1 < size) str[out_idx] = (ch); \
        out_idx++; \
    } while (0)

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
                    while (*s) EMIT_CHAR(*s++);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    EMIT_CHAR(c);
                    break;
                }
                case 'd': {
                    int32_t d = va_arg(args, int32_t);
                    if (d < 0) {
                        EMIT_CHAR('-');
                        d = -d;
                    }
                    char buf[12];
                    int i = 0;
                    if (d == 0) buf[i++] = '0';
                    while (d > 0) { buf[i++] = '0' + (d % 10); d /= 10; }
                    for (int j = i - 1; j >= 0; j--) EMIT_CHAR(buf[j]);
                    break;
                }
                case 'u': {
                    uint32_t u = va_arg(args, uint32_t);
                    char buf[12];
                    int i = 0;
                    if (u == 0) buf[i++] = '0';
                    while (u > 0) { buf[i++] = '0' + (u % 10); u /= 10; }
                    for (int j = i - 1; j >= 0; j--) EMIT_CHAR(buf[j]);
                    break;
                }
                case 'x':
                case 'X': {
                    uint32_t x = va_arg(args, uint32_t);
                    const char *hex_chars = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                    char buf[9];
                    buf[8] = '\0';
                    for (int i = 7; i >= 0; i--) { buf[i] = hex_chars[x & 0xF]; x >>= 4; }
                    int first = 7;
                    for (int i = 0; i < 8; i++) { if (buf[i] != '0') { first = i; break; } }
                    int digits = 8 - first;
                    if (digits < width) digits = width;
                    if (digits > 8) digits = 8;
                    if (digits == 0) digits = 1;
                    int start = 8 - digits;
                    if (!pad_zero && width > 0) {
                        int spaces = width - (8 - first);
                        for (int s = 0; s < spaces; s++) EMIT_CHAR(' ');
                        start = first;
                    }
                    for (int j = start; j < 8; j++) EMIT_CHAR(buf[j]);
                    break;
                }
                case '%': {
                    EMIT_CHAR('%');
                    break;
                }
                default: {
                    EMIT_CHAR('%');
                    EMIT_CHAR(*fmt);
                    break;
                }
            }
        } else {
            EMIT_CHAR(*fmt);
        }
        fmt++;
    }

    if (out_idx < size) {
        str[out_idx] = '\0';
    } else {
        str[size - 1] = '\0';
    }
    #undef EMIT_CHAR
    return (int)out_idx;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(str, size, fmt, args);
    va_end(args);
    return ret;
}

