#ifndef __LED_STRIP_UWF_H__
#define __LED_STRIP_UWF_H__

#include <stdint.h>

/* This interface controls three different single color LED strips: 
 *  - UltraViolet (U) 12V LED strip
 *  - White (W) 12V LED strip
 *  - Fito (F) 12V LED strip 
 */

void    LED_Strip_UWF_Init(void);
void    LED_Strip_U_SetBrightness(uint8_t value);
uint8_t LED_Strip_U_GetBrightness(void);
void    LED_Strip_W_SetBrightness(uint8_t value);
uint8_t LED_Strip_W_GetBrightness(void);
void    LED_Strip_F_SetBrightness(uint8_t value);
uint8_t LED_Strip_F_GetBrightness(void);

#endif /* __LED_STRIP_UWF_H__ */
