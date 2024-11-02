#ifndef __WIFI_TASK_H__
#define __WIFI_TASK_H__

#include <stdint.h>

#define WIFI_STRING_MAX_LEN  (32)

typedef struct
{
    uint8_t  length;
    char     data[WIFI_STRING_MAX_LEN + sizeof(uint32_t) - sizeof(uint8_t)];
} wifi_string_t, * wifi_string_p;

void WiFi_Task_Init(void);
bool WiFi_SaveParams(wifi_string_p p_ssid, wifi_string_p p_pswd, wifi_string_p p_site);
bool WiFi_GetParams(wifi_string_p p_ssid, wifi_string_p p_pswd, wifi_string_p p_site);

#endif /* __WIFI_TASK_H__ */
