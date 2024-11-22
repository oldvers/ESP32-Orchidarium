#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "led_task.h"
#include "wifi_task.h"
#include "time_task.h"
#include "climate_task.h"

//-------------------------------------------------------------------------------------------------

#define MAIN_LOG  1

#if (1 == MAIN_LOG)
static const char * gTAG = "MAIN";
#    define MAIN_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define MAIN_LOGE(...)  ESP_LOGE(gTAG, __VA_ARGS__)
#    define MAIN_LOGV(...)  ESP_LOGV(gTAG, __VA_ARGS__)
#else
#    define MAIN_LOGI(...)
#    define MAIN_LOGE(...)
#    define MAIN_LOGV(...)
#endif

//-------------------------------------------------------------------------------------------------

void app_main(void)
{
    MAIN_LOGI("*");
    MAIN_LOGI("--- Application Started ----------------------------------------");

    LED_Task_Init();
    WiFi_Task_Init();
    Time_Task_Init();
    Climate_Task_Init();

//---    LED_Task_Test();
//---    LED_Strip_UWF_Test();
//---    FAN_Test();
//---    Humidifier_Test();
//---    I2C_Test();
//---    Climate_Task_Test();
//---    Time_Task_Test();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    };
}

//-------------------------------------------------------------------------------------------------
