/* HTTP Server */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"

#include "types.h"
#include "httpd.h"
#include "wifi_task.h"
#include "led_task.h"
#include "time_task.h"

//-------------------------------------------------------------------------------------------------

#define HTTPS_LOG  1

#if (1 == HTTPS_LOG)
static const char * gTAG = "HTTPS";
#    define HTTPS_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define HTTPS_LOGE(...)  ESP_LOGE(gTAG, __VA_ARGS__)
#    define HTTPS_LOGW(...)  ESP_LOGV(gTAG, __VA_ARGS__)
#else
#    define HTTPS_LOGI(...)
#    define HTTPS_LOGE(...)
#    define HTTPS_LOGW(...)
#endif

//-------------------------------------------------------------------------------------------------

enum
{
    SSI_UPTIME,
    SSI_FREE_HEAP,
    SSI_LED_STATE
};

//-------------------------------------------------------------------------------------------------

static bool gConfig = false;

//-------------------------------------------------------------------------------------------------

static int32_t ssi_handler(int32_t iIndex, char * pcInsert, int32_t iInsertLen)
{
    switch (iIndex)
    {
        case SSI_UPTIME:
            snprintf(pcInsert, iInsertLen, "%lu", pdTICKS_TO_MS(xTaskGetTickCount()));
            break;
        case SSI_FREE_HEAP:
            snprintf(pcInsert, iInsertLen, "%d", 35000); // (int) xPortGetFreeHeapSize());
            break;
        case SSI_LED_STATE:
            snprintf(pcInsert, iInsertLen, "Off"); // gpio_get_level(LED_PIN) ? "Off" : "On");
            break;
        default:
            snprintf(pcInsert, iInsertLen, "N/A");
            break;
    }

    /* Tell the server how many characters to insert */
    return (strlen(pcInsert));
}

//-------------------------------------------------------------------------------------------------

static char * gpio_cgi_handler(int iIndex, int iNumParams, char * pcParam[], char * pcValue[])
{
    for (int i = 0; i < iNumParams; i++)
    {
        if (strcmp(pcParam[i], "on") == 0)
        {
//          uint8_t gpio_num = atoi(pcValue[i]);
//          gpio_enable(gpio_num, GPIO_OUTPUT);
//          gpio_write(gpio_num, true);
        }
        else if (strcmp(pcParam[i], "off") == 0)
        {
//          uint8_t gpio_num = atoi(pcValue[i]);
//          gpio_enable(gpio_num, GPIO_OUTPUT);
//          gpio_write(gpio_num, false);
        }
        else if (strcmp(pcParam[i], "toggle") == 0)
        {
//          uint8_t gpio_num = atoi(pcValue[i]);
//          gpio_enable(gpio_num, GPIO_OUTPUT);
//          gpio_toggle(gpio_num);
        }
    }
    return "/index.ssi";
}

//-------------------------------------------------------------------------------------------------

static char * complete_cgi_handler(int iIndex, int iNumParams, char * pcParam[], char * pcValue[])
{
    return "/complete.html";
}

//-------------------------------------------------------------------------------------------------

static char * websocket_cgi_handler(int iIndex, int iNumParams, char * pcParam[], char * pcValue[])
{
    return "/websockets.html";
}

//-------------------------------------------------------------------------------------------------

static void vWebSocket_Task(void * pvParameter)
{
    struct tcp_pcb * pcb = (struct tcp_pcb *) pvParameter;

    for (;;)
    {
        if (pcb == NULL || pcb->state != ESTABLISHED)
        {
            HTTPS_LOGI("Connection closed, deleting task");
            break;
        }

        int uptime = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        int heap = 35000; //(int) xPortGetFreeHeapSize();
        int led = 0; //!gpio_read(LED_PIN);

        /* Generate response in JSON format */
        char response[64];
        int len = snprintf(response, sizeof (response),
                "{\"uptime\" : \"%d\","
                " \"heap\" : \"%d\","
                " \"led\" : \"%d\"}", uptime, heap, led);
        if (len < sizeof (response))
        {
            websocket_write(pcb, (unsigned char *) response, len, WS_TEXT_MODE);
        }

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

//-------------------------------------------------------------------------------------------------

static bool websocket_parse_wifi_string(wifi_string_p p_str, uint8_t * p_buf, uint8_t * p_offset)
{
    wifi_string_p p_str_buf = NULL;

    if ((WIFI_STRING_MAX_LEN * 3) < (*p_offset)) return false;
    
    p_str_buf = (wifi_string_p)&p_buf[*p_offset];

    if (WIFI_STRING_MAX_LEN < p_str_buf->length) return false;

    memcpy(p_str, p_str_buf, (p_str_buf->length + 1));
    *p_offset += (p_str_buf->length + 1);

    return true;
}

//-------------------------------------------------------------------------------------------------

static bool websocket_copy_wifi_string(uint8_t * p_buf, uint8_t * p_offset, wifi_string_p p_str)
{
    if ((WIFI_STRING_MAX_LEN * 3) < (*p_offset)) return false;
    if (WIFI_STRING_MAX_LEN < p_str->length) return false;

    memcpy(&p_buf[*p_offset], p_str, (p_str->length + 1));
    *p_offset += (p_str->length + 1);

    return true;
}

//-------------------------------------------------------------------------------------------------
/**
 * This function is called when websocket frame is received.
 *
 * Note: this function is executed on TCP thread and should return as soon
 * as possible.
 */
static void websocket_cb(struct tcp_pcb * pcb, uint8_t * data, uint16_t data_len, uint8_t mode)
{
    enum
    {
        MAX_LEN                       = (4 + 3 * sizeof(wifi_string_t)),
        MAX_DATE_TIME_LEN             = 28,
        CMD_UNKNOWN                   = 0x00,
        CMD_GET_CONNECTION_PARAMETERS = 0x01,
        CMD_SET_CONNECTION_PARAMETERS = 0x02,
        CMD_SET_COLOR                 = 0x03,
        CMD_SET_SUN_IMITATION_MODE    = 0x04,
        CMD_GET_STATUS                = 0x05,
        SUCCESS                       = 0x00,
        ERROR                         = 0xFF,
        ON                            = 0x01,
        OFF                           = 0x00,
        MODE_SUN_IMITATION            = 0,
        MODE_COLOR                    = 1,
    };

    uint8_t        response[MAX_LEN] = {0};
    uint8_t        offset            = 1;
    uint8_t        len               = 2;
    wifi_string_t  ssid              = {0};
    wifi_string_t  pswd              = {0};
    wifi_string_t  site              = {0};
    time_message_t time_msg          = {0};
    led_color_t    color             = {0};
    time_t         now               = 0;
    struct tm      datetime          = {0};
    bool           result            = true;

    response[0] = CMD_UNKNOWN;
    response[1] = ERROR;

    switch (data[0])
    {
        case CMD_GET_CONNECTION_PARAMETERS:
            result = WiFi_GetParams(&ssid, &pswd, &site);
            if (true == result)
            {
                offset = 2;
                result &= websocket_copy_wifi_string(response, &offset, &ssid);
                result &= websocket_copy_wifi_string(response, &offset, &pswd);
                result &= websocket_copy_wifi_string(response, &offset, &site);
                if (result)
                {
                    len = offset;
                    response[0] = CMD_GET_CONNECTION_PARAMETERS;
                    response[1] = SUCCESS;
                }
            }
            break;
        case CMD_SET_CONNECTION_PARAMETERS:
            result &= websocket_parse_wifi_string(&ssid, data, &offset);
            result &= websocket_parse_wifi_string(&pswd, data, &offset);
            result &= websocket_parse_wifi_string(&site, data, &offset);
            if (result)
            {
                HTTPS_LOGI("The config received!");
                HTTPS_LOGI(" - SSID = %s", ssid.data);
                HTTPS_LOGI(" - PSWD = %s", pswd.data);
                HTTPS_LOGI(" - SITE = %s", site.data);
            }
            result &= WiFi_SaveParams(&ssid, &pswd, &site);
            if (result)
            {
                response[0] = CMD_SET_CONNECTION_PARAMETERS;
                response[1] = SUCCESS;
            }
            break;
        case CMD_SET_COLOR:
            HTTPS_LOGI("The color received - R:%d G:%d B:%d", data[1], data[2], data[3]);

            led_message_t msg =
            {
                .command   = LED_CMD_INDICATE_COLOR,
                .src_color = {.bytes = {0}},
                .dst_color = {.r = data[1], .g = data[2], .b = data[3]},
                .interval  = 0,
                .duration  = 0
            };
            LED_Task_SendMsg(&msg);

            time_msg.command = TIME_CMD_SUN_DISABLE;
            Time_Task_SendMsg(&time_msg);

            response[0] = CMD_SET_COLOR;
            response[1] = SUCCESS;
            break;
        case CMD_SET_SUN_IMITATION_MODE:
            HTTPS_LOGI("The Sun mode received: %d", data[1]);
            if (ON == data[1])
            {
                time_msg.command = TIME_CMD_SUN_ENABLE;
            }
            else
            {
                time_msg.command = TIME_CMD_SUN_DISABLE;
            }
            Time_Task_SendMsg(&time_msg);
            response[0] = CMD_SET_SUN_IMITATION_MODE;
            response[1] = SUCCESS;
            break;
        case CMD_GET_STATUS:
            response[0] = CMD_GET_STATUS;
            response[1] = SUCCESS;
            if (FW_TRUE == Time_Task_IsInSunImitationMode())
            {
                response[2] = MODE_SUN_IMITATION;
            }
            else
            {
                response[2] = MODE_COLOR;
            }
            LED_Task_GetCurrentColor(&color);
            response[3] = color.r;
            response[4] = color.g;
            response[5] = color.b;
            time(&now);
            localtime_r(&now, &datetime);
            offset = strftime((char *)&response[7], MAX_DATE_TIME_LEN, "%c", &datetime);
            response[6] = offset;
            len = (offset + 7);
            break;
        case 'A': // ADC
            /* This should be done on a separate thread in 'real' applications */
            //rnd = esp_random();
            //val = (rnd >> 22); // sdk_system_adc_read();
            break;
        case 'D': // Disable LED
            //gpio_write(LED_PIN, true);
            //val = 0xDEAD;
            break;
        case 'E': // Enable LED
            //gpio_write(LED_PIN, false);
            //val = 0xBEEF;
            break;
        default:
            HTTPS_LOGI("Unknown command");
            //val = 0;
            break;
    }

    websocket_write(pcb, response, len, WS_BIN_MODE);
}

//-------------------------------------------------------------------------------------------------
/**
 * This function is called when new websocket is open and
 * creates a new websocket_task if requested URI equals '/stream'.
 */
static void websocket_open_cb(struct tcp_pcb * pcb, const char * uri)
{
    HTTPS_LOGI("WS URI: %s", uri);
    if (!strcmp(uri, "/stream"))
    {
        HTTPS_LOGI("Request for streaming");
        xTaskCreate(&vWebSocket_Task, "WebSocket Task", 1024, (void *)pcb, 2, NULL);
    }
}

//-------------------------------------------------------------------------------------------------

static void vHTTP_Server_Task(void * pvParameters)
{
    tCGI pCGIs[] =
    {
        {"/gpio", (tCGIHandler) gpio_cgi_handler},
        {"/complete", (tCGIHandler) complete_cgi_handler},
        {"/websockets", (tCGIHandler) websocket_cgi_handler},
    };

    const char * pcConfigSSITags[] =
    {
        "uptime", // SSI_UPTIME
        "heap",   // SSI_FREE_HEAP
        "led"     // SSI_LED_STATE
    };

    /* register handlers and start the server */
    http_set_cgi_handlers(pCGIs, sizeof (pCGIs) / sizeof (pCGIs[0]));
    http_set_ssi_handler
    (
        (tSSIHandler) ssi_handler,
        pcConfigSSITags,
        sizeof (pcConfigSSITags) / sizeof (pcConfigSSITags[0])
    );
    websocket_register_callbacks
    (
        (tWsOpenHandler) websocket_open_cb,
        (tWsHandler) websocket_cb
    );
    httpd_init(gConfig);

    for (;;)
    {
        vTaskDelay(25 / portTICK_PERIOD_MS);
    }
}

//-------------------------------------------------------------------------------------------------

void HTTP_Server_Init(bool config)
{
    HTTPS_LOGI("SDK version: %s", esp_get_idf_version());

    gConfig = config;
    HTTPS_LOGI("HTTP Config = %d", gConfig);

    /* Initialize task */
    xTaskCreate(&vHTTP_Server_Task, "HTTP Server", 6144, NULL, 2, NULL);
}

//-------------------------------------------------------------------------------------------------
