#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "sdkconfig.h"
#include "led_strip.h"

#include "led_strip_rgb.h"

//-------------------------------------------------------------------------------------------------

static led_strip_handle_t gLedStrip  = {0};
static uint8_t *          gLeds      = NULL;
static uint16_t           gLedsCount = 0;

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_Init(uint8_t * leds, uint16_t count)
{
    gLeds      = leds;
    gLedsCount = count;

    /* Clear all the pixels */
    memset(gLeds, 0, gLedsCount);

    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = 
    {
        /* The GPIO that connected to the LED strip's data line */
        .strip_gpio_num = CONFIG_LED_STRIP_RGB_GPIO,
        /* The number of LEDs in the strip */
        .max_leds = ((count + 2) / 3), 
        /* Pixel format of the LED strip */
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        /* LED strip model */ 
        .led_model = LED_MODEL_WS2812,
        /* Whether to invert the output signal (useful when the hardware has a level inverter) */ 
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config =
    {
        /* 10 MHz */
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &gLedStrip));

    /* Set all LED off to clear all pixels */
    led_strip_clear(gLedStrip);

    /* Init the power pin */
    gpio_config_t drv_en_config =
    {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << CONFIG_LED_STRIP_RGB_POWER_GPIO),
    };
    ESP_ERROR_CHECK(gpio_config(&drv_en_config));
    gpio_set_level(CONFIG_LED_STRIP_RGB_POWER_GPIO, 0);
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_Update(void)
{
    uint32_t pos = 0;

    for (pos = 0; pos < gLedsCount; pos += 3)
    {
        /* Set the LED pixel using RGB from 0 (0%) to 255 (100%) for each color */
        led_strip_set_pixel(gLedStrip, (pos / 3), gLeds[pos + 1], gLeds[pos + 0], gLeds[pos + 2]);
    }
    /* Refresh the strip to send data */
    led_strip_refresh(gLedStrip);
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_SetPixelColor(uint16_t pixel, led_color_t * p_color)
{
    uint32_t pos = pixel * 3;

    if (gLedsCount <= (pos + 2)) return;

    gLeds[pos++] = p_color->g;
    gLeds[pos++] = p_color->r;
    gLeds[pos++] = p_color->b;
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_Rotate(bool direction)
{
    uint8_t led[3];

    if (true == direction)
    {
        memcpy(led, gLeds, 3);
        memmove(gLeds, gLeds + 3, gLedsCount - 3);
        memcpy(gLeds + gLedsCount - 3, led, 3);
    }
    else
    {
        memcpy(led, gLeds + gLedsCount - 3, 3);
        memmove(gLeds + 3, gLeds, gLedsCount - 3);
        memcpy(gLeds, led, 3);
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_Clear(void)
{
    memset(gLeds, 0, gLedsCount);
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_SetColor(led_color_t * p_color)
{
    uint32_t pos = 0;

    for (pos = 0; pos < gLedsCount;)
    {
        gLeds[pos++] = p_color->g;
        gLeds[pos++] = p_color->r;
        gLeds[pos++] = p_color->b;
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_GetAverageColor(led_color_t * p_color)
{
    uint16_t r = 0, g = 0, b = 0;
    uint32_t pos = 0;

    for (pos = 0; pos < gLedsCount;)
    {
        g += gLeds[pos++];
        r += gLeds[pos++];
        b += gLeds[pos++];
    }

    pos = (gLedsCount / 3);

    p_color->r = (uint8_t)(r / pos);
    p_color->g = (uint8_t)(g / pos);
    p_color->b = (uint8_t)(b / pos);
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_PowerOn(void)
{
    gpio_set_level(CONFIG_LED_STRIP_RGB_POWER_GPIO, 1);
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_PowerOff(void)
{
    gpio_set_level(CONFIG_LED_STRIP_RGB_POWER_GPIO, 0);
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_RGB_Test(void)
{
    enum
    {
        LEDS_COUNT = 18,
        DELAY = 50,
    };
    uint8_t     gLeds[3 * LEDS_COUNT] = {0};
    led_color_t color                 = {0};
    uint8_t     i                     = 0;

    LED_Strip_RGB_Init(gLeds, sizeof(gLeds));

    LED_Strip_RGB_PowerOn();
    vTaskDelay(pdMS_TO_TICKS(DELAY));
    LED_Strip_RGB_Clear();
    LED_Strip_RGB_Update();
    for (i = 0; i < LEDS_COUNT; i++)
    {
        color.r = 128;
        color.g = 0;
        color.b = 0;
        LED_Strip_RGB_SetPixelColor(i, &color);
        LED_Strip_RGB_Update();
        vTaskDelay(pdMS_TO_TICKS(DELAY));
    }
    for (i = LEDS_COUNT; i > 0; i--)
    {
        color.r = 0;
        color.g = 128;
        color.b = 0;
        LED_Strip_RGB_SetPixelColor((i - 1), &color);
        LED_Strip_RGB_Update();
        vTaskDelay(pdMS_TO_TICKS(DELAY));
    }
    for (i = 0; i < LEDS_COUNT; i++)
    {
        color.r = 0;
        color.g = 0;
        color.b = 128;
        LED_Strip_RGB_SetPixelColor(i, &color);
        LED_Strip_RGB_Update();
        vTaskDelay(pdMS_TO_TICKS(DELAY));
    }
    LED_Strip_RGB_Clear();
    LED_Strip_RGB_Update();
    LED_Strip_RGB_PowerOff();
}

//-------------------------------------------------------------------------------------------------
