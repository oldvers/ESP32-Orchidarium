#ifndef __FAN_H__
#define __FAN_H__

#include <stdint.h>

typedef enum
{
    FAN_SPEED_NONE = 0,
    FAN_SPEED_LOW,
    FAN_SPEED_MEDIUM,
    FAN_SPEED_HIGH,
    FAN_SPEED_FULL,
} fan_speed_t;

/* This interface controls the 12V FAN */

void        FAN_Init(void);
void        FAN_SetSpeed(fan_speed_t value);
fan_speed_t FAN_GetSpeed(void);
void        FAN_Test(void);

#endif /* __FAN_H__ */
