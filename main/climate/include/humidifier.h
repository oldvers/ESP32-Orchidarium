#ifndef __HUMIDIFIER_H__
#define __HUMIDIFIER_H__

#include <stdint.h>
#include <stdbool.h>

/* This interface controls the Humidifier with BME280 sensor embedded */

void     Humidifier_Init(void);
void     Humidifier_PowerOn(void);
void     Humidifier_PowerOff(void);
void     Humidifier_OnOffButtonClick(void);
bool     Humidifier_IsPoweredOn(void);
void     Humidifier_ReadSensors(void);
int16_t  Humidifier_GetTemperature(void);
uint32_t Humidifier_GetPressure(void);
uint16_t Humidifier_GetHumidity(void);
void     Humidifier_Test(void);

#endif /* __HUMIDIFIER_H__ */
