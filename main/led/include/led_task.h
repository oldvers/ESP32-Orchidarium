#ifndef __LED_TASK_H__
#define __LED_TASK_H__

#include <stdint.h>
#include "led_strip_rgb.h"

typedef enum
{
    LED_CMD_EMPTY = 0,
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
    uint8_t v;
    uint8_t a;
} led_brightness_t, * led_brightness_p;

typedef struct
{
    led_command_t command;
    union
    {
        led_color_t      color;
        led_brightness_t brightness;
    } src;
    union
    {
        led_color_t      color;
        led_brightness_t brightness;
    } dst;
    uint32_t      interval;
    uint32_t      duration;
} led_message_t, * led_message_p;

void    LED_Task_Init(void);
void    LED_Task_SendMsg(led_message_p p_msg);
void    LED_Task_DetermineColor(led_message_p p_msg, led_color_p p_color);
void    LED_Task_GetCurrentColor(led_color_p p_color);
uint8_t LED_Task_GetCurrentUltraViolet(void);
uint8_t LED_Task_GetCurrentWhite(void);
uint8_t LED_Task_GetCurrentFito(void);
void    LED_Task_Test(void);

#endif /* __LED_TASK_H__ */
