#ifndef __HUMIDIFIER_H__
#define __HUMIDIFIER_H__

#include <stdint.h>

/* This interface controls the Humidifier with BME280 sensor embedded */

void Humidifier_Init(void);
void Humidifier_PowerOn(void);
void Humidifier_PowerOff(void);
void Humidifier_OnOffButtonClick(void);
void Humidifier_Test(void);

#endif /* __HUMIDIFIER_H__ */
