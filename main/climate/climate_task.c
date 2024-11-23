#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"

#include "types.h"
#include "fan.h"
#include "humidifier.h"
#include "climate_task.h"

//-------------------------------------------------------------------------------------------------

#define CLIMATE_TASK_TICK_MS   (pdMS_TO_TICKS(1000))
#define CLIMATE_TASK_KEY       (0xFACECAFE)

#define CLT_LOG  0

#if (1 == CLT_LOG)
static const char * gTAG = "CLIMATE";
#    define CLT_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define CLT_LOGE(...)  ESP_LOGE(gTAG, __VA_ARGS__)
#    define CLT_LOGW(...)  ESP_LOGV(gTAG, __VA_ARGS__)
#else
#    define CLT_LOGI(...)
#    define CLT_LOGE(...)
#    define CLT_LOGW(...)
#endif

//-------------------------------------------------------------------------------------------------

typedef struct
{
    uint32_t interval;
    uint32_t duration;
    uint32_t counter;
    struct
    {
        uint8_t repeat : 1;
    };
} climate_time_t;

typedef struct
{
    climate_command_t command;
    fan_speed_t       speed;
    climate_time_t    time;
} fan_t;

typedef struct
{
    climate_command_t command;
    bool              on;
    climate_time_t    time;
} humidifier_t;

typedef struct
{
    time_t ten_sec;
    time_t one_min;
    time_t one_hour;
} alarms_t, * alarms_p;

typedef struct
{
    uint64_t pressure;
    uint32_t temperature;
    uint32_t humidity;
    uint32_t count;
} accumulator_t, * accumulator_p;

typedef struct
{
    climate_measurements_t     minute;
    climate_day_measurements_t day;
    uint32_t                   key;
} measurements_t, * measurements_p;

typedef struct
{
    alarms_t       alarms;
    accumulator_t  acc_one_min;
    accumulator_t  acc_one_hour;
    measurements_p meas;
} sensors_t;

//-------------------------------------------------------------------------------------------------

static measurements_t RTC_NOINIT_ATTR gMeasurements;

static QueueHandle_t gClimateQueue = {0};
static fan_t         gFAN          = {0};
static humidifier_t  gHumidifier   = {0};
static sensors_t     gSensors      = {0};

//-------------------------------------------------------------------------------------------------

static void clt_ProcessFAN(void)
{
    enum
    {
        FAN_POWER_ON_TIMEOUT = 250,
    };
    if (CLIMATE_CMD_EMPTY != gFAN.command)
    {
        if ((0 < gFAN.time.duration) && (gFAN.time.duration < gFAN.time.interval))
        {
            if (0 == gFAN.time.counter)
            {
                CLT_LOGI("FAN Speed: %d - C: %lu", gFAN.speed, gFAN.time.counter);
                FAN_SetSpeed(FAN_SPEED_FULL);
                vTaskDelay(pdMS_TO_TICKS(FAN_POWER_ON_TIMEOUT));
                FAN_SetSpeed(gFAN.speed);
            }
            else if (gFAN.time.duration == gFAN.time.counter)
            {
                CLT_LOGI("FAN Speed: 0 - C: %lu", gFAN.time.counter);
                FAN_SetSpeed(FAN_SPEED_NONE);
            }
            else if (gFAN.time.interval == gFAN.time.counter)
            {
                if (false == gFAN.time.repeat)
                {
                    CLT_LOGI("FAN Speed: 0 - C: %lu", gFAN.time.counter);
                    memset(&gFAN, 0, sizeof(gFAN));
                    FAN_SetSpeed(FAN_SPEED_NONE);
                }
                /* Let the counter to overflow in the following code */
                gFAN.time.counter = UINT32_MAX;
            }
            gFAN.time.counter++;
        }
        else
        {
            CLT_LOGI("FAN Speed: %d - Permanent", gFAN.speed);
            if (FAN_SPEED_NONE < gFAN.speed)
            {
                FAN_SetSpeed(FAN_SPEED_FULL);
                vTaskDelay(pdMS_TO_TICKS(FAN_POWER_ON_TIMEOUT));
            }
            FAN_SetSpeed(gFAN.speed);
            memset(&gFAN, 0, sizeof(gFAN));
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void clt_ProcessHumidifier(void)
{
    if (CLIMATE_CMD_EMPTY != gHumidifier.command)
    {
        if ((true == gHumidifier.on) &&
            (0 < gHumidifier.time.duration) && 
            (gHumidifier.time.duration < gHumidifier.time.interval))
        {
            if (0 == gHumidifier.time.counter)
            {
                CLT_LOGI("Humidifier: %d - C: %lu", gHumidifier.on, gHumidifier.time.counter);
                Humidifier_OnOffButtonClick();
            }
            else if (gHumidifier.time.duration == gHumidifier.time.counter)
            {
                CLT_LOGI("Humidifier: 0 - C: %lu", gHumidifier.time.counter);
                Humidifier_PowerOff();
                Humidifier_PowerOn();
            }
            else if (gHumidifier.time.interval == gHumidifier.time.counter)
            {
                if (false == gHumidifier.time.repeat)
                {
                    CLT_LOGI("Humidifier: 0 - C: %lu", gHumidifier.time.counter);
                    memset(&gHumidifier, 0, sizeof(gHumidifier));
                    Humidifier_PowerOff();
                    Humidifier_PowerOn();
                }
                /* Let the counter to overflow in the following code */
                gHumidifier.time.counter = UINT32_MAX;
            }
            gHumidifier.time.counter++;
        }
        else
        {
            CLT_LOGI("Humidifier: %d - Permanent", gHumidifier.on);
            if (true == gHumidifier.on)
            {
                Humidifier_OnOffButtonClick();
            }
            else
            {
                Humidifier_PowerOff();
                Humidifier_PowerOn();
            }
            memset(&gHumidifier, 0, sizeof(gHumidifier));
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void clt_SetSensorsTenSecondsAlarm(time_t * p_now, time_t * p_alarm)
{
    struct tm dt = {0};

    /* Set the alarm for the next 10 seconds */
    *p_alarm = (*p_now + 10);
    localtime_r(p_alarm, &dt);
    *p_alarm -= (dt.tm_sec % 10);
}

//-------------------------------------------------------------------------------------------------

static void clt_SetSensorsOneMinuteAlarm(time_t * p_now, time_t * p_alarm)
{
    struct tm dt = {0};

    /* Set the alarm for the next 1 minute */
    *p_alarm = (*p_now + 60);
    localtime_r(p_alarm, &dt);
    *p_alarm -= dt.tm_sec;
}

//-------------------------------------------------------------------------------------------------

static void clt_SetSensorsOneHourAlarm(time_t * p_now, time_t * p_alarm)
{
    struct tm dt = {0};

    /* Set the alarm for the next 1 hour */
    *p_alarm = (*p_now + 3600);
    localtime_r(p_alarm, &dt);
    *p_alarm -= (dt.tm_min * 60 + dt.tm_sec);
}

//-------------------------------------------------------------------------------------------------

static void clt_SetSensorsAlarms(void)
{
    time_t now = 0; 

    time(&now);

    clt_SetSensorsTenSecondsAlarm(&now, &gSensors.alarms.ten_sec);
    clt_SetSensorsOneMinuteAlarm(&now, &gSensors.alarms.one_min);
    clt_SetSensorsOneHourAlarm(&now, &gSensors.alarms.one_hour);
}

//-------------------------------------------------------------------------------------------------

static void clt_SensorsAcummulateMinute(void)
{
    gSensors.acc_one_min.pressure    += Humidifier_GetPressure();
    gSensors.acc_one_min.temperature += Humidifier_GetTemperature();
    gSensors.acc_one_min.humidity    += Humidifier_GetHumidity();
    gSensors.acc_one_min.count++;
}

//-------------------------------------------------------------------------------------------------

static void clt_SensorsUpdateMinuteMeasurements(void)
{
    gSensors.meas->minute.pressure =
    (
        gSensors.acc_one_min.pressure / gSensors.acc_one_min.count / 1000
    );
    gSensors.meas->minute.temperature =
    (
        gSensors.acc_one_min.temperature / gSensors.acc_one_min.count
    );
    gSensors.meas->minute.humidity =
    (
        gSensors.acc_one_min.humidity / gSensors.acc_one_min.count
    );

    CLT_LOGI
    (
        "Minute - T: %4u - H: %4u - P: %6lu",
        gSensors.meas->min.temperature,
        gSensors.meas->min.humidity,
        gSensors.meas->min.pressure
    );
}

//-------------------------------------------------------------------------------------------------

void clt_SensorsAcummulateHour(void)
{
    gSensors.acc_one_hour.pressure    += gSensors.acc_one_min.pressure;
    gSensors.acc_one_hour.temperature += gSensors.acc_one_min.temperature;
    gSensors.acc_one_hour.humidity    += gSensors.acc_one_min.humidity;
    gSensors.acc_one_hour.count       += gSensors.acc_one_min.count;
    memset(&gSensors.acc_one_min, 0, sizeof(gSensors.acc_one_min));
}

//-------------------------------------------------------------------------------------------------

void clt_SensorsUpdateHourMeasurements(void)
{
    uint8_t idx = (CLIMATE_DAY_MEASUREMENTS_COUNT - 1);
    void *  p_dst = NULL;
    void *  p_src = NULL;
    size_t  size  = 0;

    /* Free the space for the latest element */
    p_dst = &gSensors.meas->day.pressure[0];
    p_src = &gSensors.meas->day.pressure[1];
    size  = (sizeof(gSensors.meas->day.pressure) - sizeof(gSensors.meas->day.pressure[0]));
    memmove(p_dst, p_src, size);
    /* Store to the last item in array */
    gSensors.meas->day.pressure[idx] =
    (
        gSensors.acc_one_hour.pressure / gSensors.acc_one_hour.count / 1000
    );

    /* Free the space for the latest element */
    p_dst = &gSensors.meas->day.temperature[0];
    p_src = &gSensors.meas->day.temperature[1];
    size  = (sizeof(gSensors.meas->day.temperature) - sizeof(gSensors.meas->day.temperature[0]));
    memmove(p_dst, p_src, size);
    /* Store to the last item in array */
    gSensors.meas->day.temperature[idx] =
    (
        gSensors.acc_one_hour.temperature / gSensors.acc_one_hour.count
    );

    /* Free the space for the latest element */
    p_dst = &gSensors.meas->day.humidity[0];
    p_src = &gSensors.meas->day.humidity[1];
    size  = (sizeof(gSensors.meas->day.humidity) - sizeof(gSensors.meas->day.humidity[0]));
    memmove(p_dst, p_src, size);
    /* Store to the last item in array */
    gSensors.meas->day.humidity[idx] =
    (
        gSensors.acc_one_hour.humidity / gSensors.acc_one_hour.count
    );

    memset(&gSensors.acc_one_hour, 0, sizeof(gSensors.acc_one_hour));

    CLT_LOGI
    (
        "Hour - T: %4u - H: %4u - P: %6lu",
        gSensors.meas->day.temperature[idx],
        gSensors.meas->day.humidity[idx],
        gSensors.meas->day.pressure[idx]
    );
}

//-------------------------------------------------------------------------------------------------

static void clt_ProcessSensors(void)
{
    enum
    {
        TIME_STR_MAX_LEN = 28,
    };
    time_t    now                      = 0;
    struct tm datetime                 = {0};
    char      string[TIME_STR_MAX_LEN] = {0};

    /* Update the 'now' variable with current time */
    time(&now);

    localtime_r(&now, &datetime);
    strftime(string, sizeof(string), "%c", &datetime);

    if (now >= gSensors.alarms.ten_sec)
    {
        CLT_LOGI("Alarm 10 S! - %s", string);
        clt_SetSensorsTenSecondsAlarm(&now, &gSensors.alarms.ten_sec);

        /* Read sensors' values */
        Humidifier_ReadSensors();

        /* Accumulate the minute values */
        clt_SensorsAcummulateMinute();

        if (now >= gSensors.alarms.one_min)
        {
            CLT_LOGI("Alarm 1 MM! - %s", string);
            clt_SetSensorsOneMinuteAlarm(&now, &gSensors.alarms.one_min);

            /* Update the minute measurements */
            clt_SensorsUpdateMinuteMeasurements();

            /* Accumulate the hour values */
            clt_SensorsAcummulateHour();

            if (now >= gSensors.alarms.one_hour)
            {
                CLT_LOGI("Alarm 1 HH! - %s", string);
                clt_SetSensorsOneHourAlarm(&now, &gSensors.alarms.one_hour);

                /* Update the hour measurements */
                clt_SensorsUpdateHourMeasurements();
            }
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void clt_ProcessMsg(climate_message_p p_msg)
{
    switch (p_msg->command)
    {
        case CLIMATE_CMD_FAN:
            memset(&gFAN, 0, sizeof(gFAN));
            gFAN.command       = p_msg->command;
            gFAN.speed         = p_msg->speed;
            gFAN.time.interval = (p_msg->interval / 1000);
            gFAN.time.duration = (p_msg->duration / 1000);
            gFAN.time.repeat   = p_msg->repeat;
            clt_ProcessFAN();
            break;
        case CLIMATE_CMD_HUMIDIFY:
            memset(&gHumidifier, 0, sizeof(gHumidifier));
            gHumidifier.command       = p_msg->command;
            gHumidifier.on            = p_msg->on;
            gHumidifier.time.interval = (p_msg->interval / 1000);
            gHumidifier.time.duration = (p_msg->duration / 1000);
            gHumidifier.time.repeat   = p_msg->repeat;
            clt_ProcessHumidifier();
            break;
        default:
            break;
    }
}

//-------------------------------------------------------------------------------------------------

void clt_Sensors_Init(void)
{
    /* Initialize the measurements if needed */
    if (CLIMATE_TASK_KEY != gMeasurements.key)
    {
        memset(&gMeasurements, 0, sizeof(gMeasurements));
        gMeasurements.key = CLIMATE_TASK_KEY;
    }

    /* Initialize the link to the measurements */
    gSensors.meas = &gMeasurements;
}

//-------------------------------------------------------------------------------------------------

static void vClimate_Task(void * pvParameters)
{
    BaseType_t        status = pdFAIL;
    climate_message_t msg    = {0};

    CLT_LOGI("Climate Task started...");

    FAN_Init();
    Humidifier_Init();
    Humidifier_PowerOn();

    clt_Sensors_Init();
    clt_SetSensorsAlarms();

    while (FW_TRUE)
    {
        status = xQueueReceive(gClimateQueue, (void *)&msg, CLIMATE_TASK_TICK_MS);

        if (pdTRUE == status)
        {
            clt_ProcessMsg(&msg);
        }
        else
        {
            clt_ProcessFAN();
            clt_ProcessHumidifier();
            clt_ProcessSensors();
        }
    }
}

//-------------------------------------------------------------------------------------------------

void Climate_Task_Init(void)
{
    gClimateQueue = xQueueCreate(20, sizeof(climate_message_t));

    (void)xTaskCreatePinnedToCore(vClimate_Task, "Climate", 4096, NULL, 2, NULL, CORE0);
}

//-------------------------------------------------------------------------------------------------

void Climate_Task_SendMsg(climate_message_p p_msg)
{
    (void)xQueueSendToBack(gClimateQueue, (void *)p_msg, (TickType_t)0);
}

//-------------------------------------------------------------------------------------------------

fan_speed_t Climate_Task_GetFanSpeed(void)
{
    /* This call is not thread safe but this is acceptable */
    return FAN_GetSpeed();
}

//-------------------------------------------------------------------------------------------------

bool Climate_Task_IsHumidifierOn(void)
{
    /* This call is not thread safe but this is acceptable */
    return Humidifier_IsPoweredOn();
}

//-------------------------------------------------------------------------------------------------

void Climate_Task_GetMeasurements(climate_measurements_p p_meas)
{
    /* This call is not thread safe but this is acceptable */
    memcpy(p_meas, &gSensors.meas->minute, sizeof(gSensors.meas->minute));
}

//-------------------------------------------------------------------------------------------------

void Climate_Task_GetDayMeasurements(climate_day_measurements_p p_meas)
{
    /* This call is not thread safe but this is acceptable */
    memcpy(p_meas, &gSensors.meas->day, sizeof(gSensors.meas->day));
}

//-------------------------------------------------------------------------------------------------
//--- Tests ---------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

void clt_TestFAN(void)
{
    climate_message_t msg = {0};

    /* Permanently turn the FAN on for 5 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_FAN;
    msg.speed       = FAN_SPEED_MEDIUM;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Permanently turn the FAN off for 5 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_FAN;
    msg.speed       = FAN_SPEED_NONE;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Turn the FAN on for 3 seconds and off for 10 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_FAN;
    msg.speed       = FAN_SPEED_FULL;
    msg.duration    = 3000;
    msg.interval    = (msg.duration + 10000);
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(msg.interval));

    /* Turn the FAN on for 3 seconds and off for 5 seconds */
    /* Repeat 3 times */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_FAN;
    msg.speed       = FAN_SPEED_HIGH;
    msg.duration    = 3000;
    msg.interval    = (msg.duration + 5000);
    msg.repeat      = true;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(3 * msg.interval));

    /* Permanently turn the FAN off for 5 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_FAN;
    msg.speed       = FAN_SPEED_NONE;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

void clt_TestHumidifier(void)
{
    climate_message_t msg = {0};

    /* Permanently turn the Humidifier on for 5 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_HUMIDIFY;
    msg.on          = true;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Permanently turn the Humidifier off for 5 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_HUMIDIFY;
    msg.on          = false;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Turn the Humidifier on for 3 seconds and off for 10 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_HUMIDIFY;
    msg.on          = true;
    msg.duration    = 3000;
    msg.interval    = (msg.duration + 10000);
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(msg.interval));

    /* Turn the Humidifier on for 3 seconds and off for 5 seconds */
    /* Repeat 3 times */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_HUMIDIFY;
    msg.on          = true;
    msg.duration    = 3000;
    msg.interval    = (msg.duration + 5000);
    msg.repeat      = true;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(4 * msg.interval));

    /* Permanently turn the Humidifier off for 5 seconds */
    memset(&msg, 0, sizeof(msg));
    msg.command     = CLIMATE_CMD_HUMIDIFY;
    msg.on          = false;
    Climate_Task_SendMsg(&msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

void Climate_Task_Test(void)
{
    /* Wait till the task initializes all the pripherals */
    vTaskDelay(pdMS_TO_TICKS(5000));

    clt_TestFAN();
    clt_TestHumidifier();
}

//-------------------------------------------------------------------------------------------------
