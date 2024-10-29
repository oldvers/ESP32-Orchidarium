#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "led_strip_rgb.h"
#include "led_strip_uwf.h"

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
    uint8_t step = 0;

    MAIN_LOGI("*");
    MAIN_LOGI("--- Application Started ----------------------------------------");

    LED_Strip_RGB_Init(gLeds, sizeof(gLeds));

    LED_Strip_RGB_SetPixelColor(0, &color);
    LED_Strip_RGB_Update();

    for (step = 0; step < 35; step++)
    {
        vTaskDelay(50 / portTICK_PERIOD_MS);
        LED_Strip_RGB_Rotate(true);
        LED_Strip_RGB_Update();
    }

    LED_Strip_UWF_Init();

    LED_Strip_U_SetBrightness(UINT8_MAX);
    LED_Strip_W_SetBrightness(UINT8_MAX);
    LED_Strip_F_SetBrightness(UINT8_MAX);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    LED_Strip_U_SetBrightness(0);
    LED_Strip_W_SetBrightness(0);
    LED_Strip_F_SetBrightness(0);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    for (step = 0; step < 40; step++)
    {
        LED_Strip_U_SetBrightness(6 * step);
        LED_Strip_W_SetBrightness(6 * step);
        LED_Strip_F_SetBrightness(6 * step);
        vTaskDelay(30 / portTICK_PERIOD_MS);
    }

    while (1) {};
}

//-------------------------------------------------------------------------------------------------
