#ifndef __LED_TASK_H__
#define __LED_TASK_H__

#include <stdint.h>
#include "led_strip_rgb.h"

typedef enum
{
    LED_CMD_EMPTY,
    LED_CMD_INDICATE_COLOR,
    LED_CMD_INDICATE_RGB_CIRCULATION,
    LED_CMD_INDICATE_FADE,
    LED_CMD_INDICATE_PINGPONG,
    LED_CMD_INDICATE_RAINBOW_CIRCULATION,
    LED_CMD_INDICATE_RAINBOW,
    LED_CMD_INDICATE_SINE,
    LED_CMD_SWITCH_OFF,
} led_command_t;

typedef struct
{
    led_command_t command;
    led_color_t   src_color;
    led_color_t   dst_color;
    uint32_t      interval;
    uint32_t      duration;
} led_message_t;

#define LED_TASK_PIXELS_COUNT (18)

void LED_Task_Init(void);
void LED_Task_SendMsg(led_message_t * p_msg);
void LED_Task_DetermineColor(led_message_t * p_msg, led_color_t * p_color);
void LED_Task_GetCurrentColor(led_color_t * p_color);
void LED_Task_Test(void);

#endif /* __LED_TASK_H__ */
