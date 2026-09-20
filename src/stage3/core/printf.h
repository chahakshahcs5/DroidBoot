#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

int vsnprintf(char *str, size_t size, const char *fmt, va_list args);
int snprintf(char *str, size_t size, const char *fmt, ...);

void printk(const char *fmt, ...);
void vprintk(const char *fmt, va_list args);

void log_info(const char *tag, const char *fmt, ...);
void log_error(const char *tag, const char *fmt, ...);

#endif // PRINTF_H
