#ifndef __FAN_H__
#define __FAN_H__

#include <stdint.h>

enum
{
    FAN_SPEED_NONE   = 0,
    FAN_SPEED_LOW    = 70,
    FAN_SPEED_MEDIUM = 106,
    FAN_SPEED_HIGH   = 178,
    FAN_SPEED_FULL   = 255,
};

/* This interface controls the 12V FAN */

void    FAN_Init(void);
void    FAN_SetSpeed(uint8_t value);
uint8_t FAN_GetSpeed(void);
void    FAN_Test(void);

#endif /* __FAN_H__ */
