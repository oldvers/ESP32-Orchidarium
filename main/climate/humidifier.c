#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "sdkconfig.h"

#include "humidifier.h"

//-------------------------------------------------------------------------------------------------

#define HUMIDIFIER_POWER_DELAY  (200)
#define HUMIDIFIER_CLICK_DELAY  (50)

//-------------------------------------------------------------------------------------------------

void Humidifier_Init(void)
{
    /* Init the power/button pins */
    gpio_config_t humidifier_gpio_config =
    {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 
        (
            (1ULL << CONFIG_HUMIDIFIER_POWER_GPIO) |
            (1ULL << CONFIG_HUMIDIFIER_BUTTON_GPIO)
        ),
    };
    ESP_ERROR_CHECK(gpio_config(&humidifier_gpio_config));
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 0);
    gpio_set_level(CONFIG_HUMIDIFIER_BUTTON_GPIO, 0);
}

//-------------------------------------------------------------------------------------------------

void Humidifier_PowerOn(void)
{
    /* Power on the humidifier */
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(HUMIDIFIER_POWER_DELAY));
}

//-------------------------------------------------------------------------------------------------

void Humidifier_PowerOff(void)
{
    /* Power off the humidifier */
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(HUMIDIFIER_POWER_DELAY));
}

//-------------------------------------------------------------------------------------------------

void Humidifier_OnOffButtonClick(void)
{
    /* Click the button */
    gpio_set_level(CONFIG_HUMIDIFIER_BUTTON_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(HUMIDIFIER_CLICK_DELAY));
    gpio_set_level(CONFIG_HUMIDIFIER_BUTTON_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(HUMIDIFIER_CLICK_DELAY));
}

//-------------------------------------------------------------------------------------------------

void Humidifier_Test(void)
{
    /* On */
    Humidifier_Init();
    Humidifier_PowerOn();
    Humidifier_OnOffButtonClick();
    /* Humidify */
    vTaskDelay(pdMS_TO_TICKS(10000));
    /* Off */
    Humidifier_PowerOff();
}

//-------------------------------------------------------------------------------------------------
