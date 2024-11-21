#ifndef __LED_STRIP_RGB_H__
#define __LED_STRIP_RGB_H__

#include <stdint.h>

typedef union
{
    struct
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };
    uint8_t  bytes[4];
    uint32_t dword;
} led_color_t, * led_color_p;

void LED_Strip_RGB_Init(uint8_t * leds, uint16_t count);
void LED_Strip_RGB_Update(void);
void LED_Strip_RGB_SetPixelColor(uint16_t pixel, led_color_p p_color);
void LED_Strip_RGB_Rotate(bool direction);
void LED_Strip_RGB_Clear(void);
void LED_Strip_RGB_SetColor(led_color_p p_color);
void LED_Strip_RGB_GetAverageColor(led_color_p p_color);
void LED_Strip_RGB_PowerOn(void);
void LED_Strip_RGB_PowerOff(void);
void LED_Strip_RGB_Test(void);

#endif /* __LED_STRIP_RGB_H__ */
