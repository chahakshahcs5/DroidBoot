#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#ifdef DEBUG_BUILD
#define IS_DEBUG_BUILD 1
#else
#define IS_DEBUG_BUILD 0
#endif

void printk(const char *fmt, ...);
void vprintk(const char *fmt, va_list args);

void log_info(const char *tag, const char *fmt, ...);
void log_error(const char *tag, const char *fmt, ...);
#define log_warn log_info

#if IS_DEBUG_BUILD
void log_debug(const char *tag, const char *fmt, ...);
#else
static inline void log_debug(const char *tag, const char *fmt, ...) {
    (void)tag;
    (void)fmt;
}
#endif

int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list args);

#endif // PRINTF_H
