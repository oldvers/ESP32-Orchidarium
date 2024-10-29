#ifndef __FAN_H__
#define __FAN_H__

#include <stdint.h>

/* This interface controls the 12V FAN */

void    FAN_Init(void);
void    FAN_SetSpeed(uint8_t value);
uint8_t FAN_GetSpeed(void);

#endif /* __FAN_H__ */
