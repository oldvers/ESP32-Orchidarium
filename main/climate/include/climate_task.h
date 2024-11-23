#ifndef __CLIMATE_TASK_H__
#define __CLIMATE_TASK_H__

#include <stdint.h>
#include "fan.h"

enum
{
    CLIMATE_DAY_MEASUREMENTS_COUNT = 24,
};

typedef enum
{
    CLIMATE_CMD_EMPTY = 0,
    CLIMATE_CMD_FAN,
    CLIMATE_CMD_HUMIDIFY,
} climate_command_t;

typedef struct
{
    uint32_t pressure;
    uint16_t temperature;
    uint16_t humidity;
} climate_measurements_t, * climate_measurements_p;

typedef struct
{
    uint32_t pressure[CLIMATE_DAY_MEASUREMENTS_COUNT];
    uint16_t temperature[CLIMATE_DAY_MEASUREMENTS_COUNT];
    uint16_t humidity[CLIMATE_DAY_MEASUREMENTS_COUNT];
} climate_day_measurements_t, * climate_day_measurements_p;

typedef struct
{
    climate_command_t command;
    struct
    {
        uint8_t repeat : 1;
    };
    union
    {
        fan_speed_t speed;
        bool        on;
    };
    uint32_t interval;
    uint32_t duration;
} climate_message_t, * climate_message_p;

void        Climate_Task_Init(void);
void        Climate_Task_SendMsg(climate_message_p p_msg);
fan_speed_t Climate_Task_GetFanSpeed(void);
bool        Climate_Task_IsHumidifierOn(void);
void        Climate_Task_GetMeasurements(climate_measurements_p p_meas);
void        Climate_Task_GetDayMeasurements(climate_day_measurements_p p_meas);
bool        Climate_Task_IsNewDayMeasurementsAvailable(void);
void        Climate_Task_Test(void);

#endif /* __CLIMATE_TASK_H__ */
