#include "driver/mcpwm_prelude.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fan.h"

//-------------------------------------------------------------------------------------------------

#define FAN_MCPWM_TIMER_RESOLUTION_HZ 1000000 /* 1 MHz, 1 tick = 1 us */
#define FAN_MCPWM_PERIOD              65000   /* 65000 us, 65 ms, 15.38 Hz */
#define FAN_MCPWM_DUTY                27000   /* 27000 us, 27 ms */

//-------------------------------------------------------------------------------------------------

enum
{
    FAN_GROUP_ID = 1,
};

//-------------------------------------------------------------------------------------------------

static mcpwm_cmpr_handle_t gComparator = NULL;
static mcpwm_gen_handle_t  gGenerator  = NULL;
static fan_speed_t         gSpeed      = FAN_SPEED_NONE;

static uint8_t gSpeeds[] =
{
    [FAN_SPEED_NONE]   = 0,
    [FAN_SPEED_LOW]    = 75,
    [FAN_SPEED_MEDIUM] = 106,
    [FAN_SPEED_HIGH]   = 178,
    [FAN_SPEED_FULL]   = 255,
};

//-------------------------------------------------------------------------------------------------

void FAN_Init(void)
{
    /* Create timer and operator */
    mcpwm_timer_handle_t timer = NULL;
    mcpwm_timer_config_t timer_config =
    {
        .group_id      = FAN_GROUP_ID,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = FAN_MCPWM_TIMER_RESOLUTION_HZ,
        .period_ticks  = FAN_MCPWM_PERIOD,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &timer));

    mcpwm_oper_handle_t operator = NULL;
    mcpwm_operator_config_t operator_config =
    {
        /* Operator must be in the same group to the timer */
        .group_id = FAN_GROUP_ID,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &operator));

    /* Connect timer and operator */
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(operator, timer));

    /* Create comparator and generator from the operator */
    mcpwm_comparator_config_t comparator_config =
    {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(operator, &comparator_config, &gComparator));

    mcpwm_generator_config_t generator_config =
    {
        .gen_gpio_num = CONFIG_FAN_GPIO,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(operator, &generator_config, &gGenerator));

    /* Set the initial compare value, so that the FAN will spin */
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(gComparator, FAN_MCPWM_DUTY));

    /* Set generator action on timer and compare event */
    /* Go high on counter empty */
    ESP_ERROR_CHECK
    (
        mcpwm_generator_set_action_on_timer_event
        (
            gGenerator,
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
            gGenerator,
            MCPWM_GEN_COMPARE_EVENT_ACTION
            (
                MCPWM_TIMER_DIRECTION_UP,
                gComparator,
                MCPWM_GEN_ACTION_LOW
            )
        )
    );

    /* Turn off all the FAN */
    ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerator, 0, true));

    /* Start the MCPWM timer */
    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
}

//-------------------------------------------------------------------------------------------------

void FAN_SetSpeed(fan_speed_t value)
{
    uint32_t duty = 0;

    gSpeed = value;

    if (FAN_SPEED_NONE == value)
    {
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerator, 0, true));
    }
    else if (FAN_SPEED_FULL == value)
    {
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerator, 1, true));
    }
    else
    {
        duty = (gSpeeds[value] * FAN_MCPWM_PERIOD / UINT8_MAX);
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(gComparator, duty));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gGenerator, -1, true));
    }
}

//-------------------------------------------------------------------------------------------------

fan_speed_t FAN_GetSpeed(void)
{
    return gSpeed;
}

//-------------------------------------------------------------------------------------------------

void FAN_Test(void)
{
    FAN_Init();
    FAN_SetSpeed(FAN_SPEED_FULL);
    vTaskDelay(pdMS_TO_TICKS(5000));
    FAN_SetSpeed(FAN_SPEED_NONE);
    vTaskDelay(pdMS_TO_TICKS(5000));
    FAN_SetSpeed(FAN_SPEED_MEDIUM);
    vTaskDelay(pdMS_TO_TICKS(5000));
    FAN_SetSpeed(FAN_SPEED_HIGH);
    vTaskDelay(pdMS_TO_TICKS(5000));
    FAN_SetSpeed(FAN_SPEED_LOW);
    vTaskDelay(pdMS_TO_TICKS(10000));
    FAN_SetSpeed(FAN_SPEED_NONE);
    vTaskDelay(pdMS_TO_TICKS(300));
}

//-------------------------------------------------------------------------------------------------
