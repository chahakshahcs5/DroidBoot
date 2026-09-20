#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>
#include <stdint.h>

void printk(const char *fmt, ...);
void vprintk(const char *fmt, va_list args);

void log_info(const char *tag, const char *fmt, ...);
void log_error(const char *tag, const char *fmt, ...);

#endif // PRINTF_H
