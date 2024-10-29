#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "led_strip_rgb.h"

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

static uint8_t gLeds[3 * 16] = {0};

//-------------------------------------------------------------------------------------------------

void app_main(void)
{
    led_color_t color = {{255, 0, 0, 0}};

    MAIN_LOGI("*");
    MAIN_LOGI("--- Application Started ----------------------------------------");

    LED_Strip_RGB_Init(gLeds, sizeof(gLeds));

    LED_Strip_RGB_SetPixelColor(0, &color);
    LED_Strip_RGB_Update();

    while (1)
    {
        vTaskDelay(50 / portTICK_PERIOD_MS);
        LED_Strip_RGB_Rotate(true);
        LED_Strip_RGB_Update();
    }
}

//-------------------------------------------------------------------------------------------------
