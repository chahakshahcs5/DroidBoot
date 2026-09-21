#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>

void     timer_init(void);
uint64_t timer_rdtsc(void);
void     timer_udelay(uint32_t us);
void     timer_mdelay(uint32_t ms);
uint32_t timer_get_ms(void);

#endif // TIMER_H
