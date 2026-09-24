#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

void Shell_Init(void);
void Shell_RxFromIsr(const uint8_t *data, uint32_t length);
void Shell_Task(void *argument);

#endif
