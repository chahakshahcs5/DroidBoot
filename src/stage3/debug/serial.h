#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stdbool.h>

void serial_init(void);
bool serial_is_present(void);
void serial_putchar(char c);
void serial_puts(const char *s);

#endif // SERIAL_H
