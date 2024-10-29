#include "driver/mcpwm_prelude.h"

#include "led_strip_uwf.h"

//-------------------------------------------------------------------------------------------------

#define LED_MCPWM_TIMER_RESOLUTION_HZ 1000000 /* 1 MHz, 1 tick = 1 us */
#define LED_MCPWM_PERIOD              1000    /* 1000 us, 1 kHz */

//-------------------------------------------------------------------------------------------------

enum
{
    IDX_LED_STRIP_U = 0,
    IDX_LED_STRIP_W,
    IDX_LED_STRIP_F,
    IDX_LED_STRIP_MAX
};

enum
{
    LED_STRIP_GROUP_ID = 0,
};

//-------------------------------------------------------------------------------------------------

static mcpwm_cmpr_handle_t gComparators[IDX_LED_STRIP_MAX] = {0};
static mcpwm_gen_handle_t  gGenerators[IDX_LED_STRIP_MAX] = {0};
static uint8_t             gBrightness[IDX_LED_STRIP_MAX] = {0};

//-------------------------------------------------------------------------------------------------

static void led_Strip_SetBrightness(uint8_t idx, uint8_t value)
{
    uint32_t duty = 0;

    gBrightness[idx] = value;

    if (0 == value)
    {
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerators[idx], 0, true));
    }
    else if (UINT8_MAX == value)
    {
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerators[idx], 1, true));
    }
    else
    {
        duty = (value * LED_MCPWM_PERIOD / UINT8_MAX);
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(gComparators[idx], duty));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerators[idx], -1, true));
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_UWF_Init(void)
{
    uint8_t idx = 0;

    /* Create MCPWM timer */
    mcpwm_timer_handle_t timer = NULL;
    mcpwm_timer_config_t timer_config =
    {
        .group_id = LED_STRIP_GROUP_ID,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = LED_MCPWM_TIMER_RESOLUTION_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = LED_MCPWM_PERIOD,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &timer));

    /* Create MCPWM operator */
    mcpwm_oper_handle_t operators[IDX_LED_STRIP_MAX];
    mcpwm_operator_config_t operator_config =
    {
        .group_id = LED_STRIP_GROUP_ID,
    };
    for (idx = 0; idx < IDX_LED_STRIP_MAX; idx++)
    {
        ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &operators[idx]));
    }

    /* Connect operators to the same timer */
    for (idx = 0; idx < IDX_LED_STRIP_MAX; idx++)
    {
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(operators[idx], timer));
    }

    /* Create comparators */
    mcpwm_comparator_config_t compare_config =
    {
        .flags.update_cmp_on_tez = true,
    };
    for (idx = 0; idx < IDX_LED_STRIP_MAX; idx++)
    {
        ESP_ERROR_CHECK
        (
            mcpwm_new_comparator(operators[idx], &compare_config, &gComparators[idx])
        );
        /* Set compare value to 0 */
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(gComparators[idx], 0));
    }

    /* Create PWM generators */
    mcpwm_generator_config_t gen_config = {0};
    const int gen_gpios[IDX_LED_STRIP_MAX] =
    {
        CONFIG_LED_STRIP_U_GPIO,
        CONFIG_LED_STRIP_W_GPIO,
        CONFIG_LED_STRIP_F_GPIO
    };
    for (idx = 0; idx < IDX_LED_STRIP_MAX; idx++)
    {
        gen_config.gen_gpio_num = gen_gpios[idx];
        ESP_ERROR_CHECK(mcpwm_new_generator(operators[idx], &gen_config, &gGenerators[idx]));
    }

    /* Set generator actions */
    for (idx = 0; idx < IDX_LED_STRIP_MAX; idx++)
    {
        /* Go high on counter empty */
        ESP_ERROR_CHECK
        (
            mcpwm_generator_set_action_on_timer_event
            (
                gGenerators[idx],
                MCPWM_GEN_TIMER_EVENT_ACTION
                (
                    MCPWM_TIMER_DIRECTION_UP, 
                    MCPWM_TIMER_EVENT_EMPTY,
                    MCPWM_GEN_ACTION_HIGH
                )
            )
        );
        /* Go low on compare threshold */
        ESP_ERROR_CHECK
        (
            mcpwm_generator_set_action_on_compare_event
            (
                gGenerators[idx],
                MCPWM_GEN_COMPARE_EVENT_ACTION
                (
                    MCPWM_TIMER_DIRECTION_UP,
                    gComparators[idx],
                    MCPWM_GEN_ACTION_LOW
                )
            )
        );
    }

    /* Turn off all the gates */
    for (idx = 0; idx < IDX_LED_STRIP_MAX; idx++)
    {
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerators[idx], 0, true));
    }

    /* Start the MCPWM timer */
    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_U_SetBrightness(uint8_t value)
{
    led_Strip_SetBrightness(IDX_LED_STRIP_U, value);
}

//-------------------------------------------------------------------------------------------------

uint8_t LED_Strip_U_GetBrightness(void)
{
    return gBrightness[IDX_LED_STRIP_U];
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_W_SetBrightness(uint8_t value)
{
    led_Strip_SetBrightness(IDX_LED_STRIP_W, value);
}

//-------------------------------------------------------------------------------------------------

uint8_t LED_Strip_W_GetBrightness(void)
{
    return gBrightness[IDX_LED_STRIP_W];
}

//-------------------------------------------------------------------------------------------------

void LED_Strip_F_SetBrightness(uint8_t value)
{
    led_Strip_SetBrightness(IDX_LED_STRIP_F, value);
}

//-------------------------------------------------------------------------------------------------

uint8_t LED_Strip_F_GetBrightness(void)
{
    return gBrightness[IDX_LED_STRIP_F];
}

//-------------------------------------------------------------------------------------------------
