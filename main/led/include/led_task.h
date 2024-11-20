#ifndef __LED_TASK_H__
#define __LED_TASK_H__

#include <stdint.h>
#include "led_strip_rgb.h"

typedef enum
{
    LED_CMD_EMPTY,
    LED_CMD_RGB_INDICATE_COLOR,
    LED_CMD_RGB_INDICATE_RGB_CIRCULATION,
    LED_CMD_RGB_INDICATE_FADE,
    LED_CMD_RGB_INDICATE_PINGPONG,
    LED_CMD_RGB_INDICATE_RAINBOW_CIRCULATION,
    LED_CMD_RGB_INDICATE_RAINBOW,
    LED_CMD_RGB_INDICATE_SINE,
    LED_CMD_RGB_SWITCH_OFF,
    LED_CMD_UV_INDICATE_BRIGHTNESS,
    LED_CMD_UV_INDICATE_SINE,
    LED_CMD_W_INDICATE_BRIGHTNESS,
    LED_CMD_W_INDICATE_SINE,
    LED_CMD_F_INDICATE_BRIGHTNESS,
    LED_CMD_F_INDICATE_SINE,
} led_command_t;

typedef struct
{
    led_command_t command;
    led_color_t   src_color;
    led_color_t   dst_color;
    uint32_t      interval;
    uint32_t      duration;
} led_message_t;

void    LED_Task_Init(void);
void    LED_Task_SendMsg(led_message_t * p_msg);
void    LED_Task_DetermineColor(led_message_t * p_msg, led_color_t * p_color);
void    LED_Task_GetCurrentColor(led_color_t * p_color);
uint8_t LED_Task_GetCurrentUltraViolet(void);
uint8_t LED_Task_GetCurrentWhite(void);
uint8_t LED_Task_GetCurrentFito(void);
void    LED_Task_Test(void);

#endif /* __LED_TASK_H__ */
