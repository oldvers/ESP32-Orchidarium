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
#include "climate_task.h"

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

#define OFFSET_OF(t,f) ((unsigned int)&(((t)0)->f))

//-------------------------------------------------------------------------------------------------

enum
{
    SSI_UPTIME,
    SSI_FREE_HEAP,
    SSI_LED_STATE
};

enum
{
    CMD_UNKNOWN                   = 0x00,
    CMD_GET_CONNECTION_PARAMETERS = 0x01,
    CMD_SET_CONNECTION_PARAMETERS = 0x02,
    CMD_SET_COLOR                 = 0x03,
    CMD_SET_SUN_IMITATION_MODE    = 0x04,
    CMD_GET_STATUS                = 0x05,
    CMD_SET_ULTRAVIOLET           = 0x06,
    CMD_SET_WHITE                 = 0x07,
    CMD_SET_FITO                  = 0x08,
    CMD_SET_FAN                   = 0x09,
    CMD_SET_HUMIDIFIER            = 0x0A,
    CMD_GET_DAY_MEASUREMENTS      = 0x0B,
    SUCCESS                       = 0x00,
    ERROR                         = 0xFF,
    ON                            = 0x01,
    OFF                           = 0x00,
    MODE_SUN_IMITATION            = 0,
    MODE_COLOR                    = 1,
};

#pragma pack(push, 1)

typedef struct
{
    uint8_t params[3 * sizeof(wifi_string_t)];
} ctrl_conn_params_t;

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ctrl_color_t;

typedef struct
{
    uint8_t       mode;
    ctrl_color_t  color;
    uint8_t       ultraviolet;
    uint8_t       white;
    uint8_t       fito;
    uint8_t       fan;
    uint8_t       humidifier;
    uint32_t      pressure;
    uint16_t      temperature;
    uint16_t      humidity;
    wifi_string_t datetime;
} ctrl_status_t;

typedef climate_day_measurements_t ctrl_day_measmts_t;

typedef struct
{
    uint8_t command;
    union
    {
        ctrl_conn_params_t conn;
        ctrl_color_t       color;
        uint8_t            value;
    };
} ctrl_req_t, * ctrl_req_p;

typedef struct
{
    uint8_t command;
    uint8_t result;
    union
    {
        ctrl_conn_params_t conn;
        ctrl_status_t      status;
        ctrl_day_measmts_t day_measmts;
    };
} ctrl_rsp_t, * ctrl_rsp_p;

#pragma pack(pop)

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

static void ctrl_GetConnParams(ctrl_rsp_p p_rsp, uint16_t * p_rsp_len)
{
    wifi_string_t  ssid   = {0};
    wifi_string_t  pswd   = {0};
    wifi_string_t  site   = {0};
    uint8_t        offset = 0;
    bool           result = WiFi_GetParams(&ssid, &pswd, &site);

    if (true == result)
    {
        result &= websocket_copy_wifi_string(p_rsp->conn.params, &offset, &ssid);
        result &= websocket_copy_wifi_string(p_rsp->conn.params, &offset, &pswd);
        result &= websocket_copy_wifi_string(p_rsp->conn.params, &offset, &site);
        if (result)
        {
            *p_rsp_len     = (offset + 2);
            p_rsp->command = CMD_GET_CONNECTION_PARAMETERS;
            p_rsp->result  = SUCCESS;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetConnParams(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    wifi_string_t  ssid   = {0};
    wifi_string_t  pswd   = {0};
    wifi_string_t  site   = {0};
    uint8_t        offset = 0;
    bool           result = true;

    result &= websocket_parse_wifi_string(&ssid, p_req->conn.params, &offset);
    result &= websocket_parse_wifi_string(&pswd, p_req->conn.params, &offset);
    result &= websocket_parse_wifi_string(&site, p_req->conn.params, &offset);
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
        p_rsp->command = CMD_SET_CONNECTION_PARAMETERS;
        p_rsp->result  = SUCCESS;
    }
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetColor(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    HTTPS_LOGI
    (
        "The color received - R:%d G:%d B:%d",
        p_req->color.r,
        p_req->color.g,
        p_req->color.b
    );

    led_message_t led_msg =
    {
        .command   = LED_CMD_RGB_INDICATE_COLOR,
        .src_color = {.bytes = {0}},
        .dst_color = {.r = p_req->color.r, .g = p_req->color.g, .b = p_req->color.b},
        .interval  = 0,
        .duration  = 0
    };
    LED_Task_SendMsg(&led_msg);

    time_message_t time_msg =
    {
        .command = TIME_CMD_SUN_DISABLE,
    };
    Time_Task_SendMsg(&time_msg);

    p_rsp->command = CMD_SET_COLOR;
    p_rsp->result  = SUCCESS;
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetSunImitationMode(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    HTTPS_LOGI("The Sun mode received: %d", p_req->value);
    time_message_t time_msg = {0};
    if (ON == p_req->value)
    {
        time_msg.command = TIME_CMD_SUN_ENABLE;
    }
    else
    {
        time_msg.command = TIME_CMD_SUN_DISABLE;
    }
    Time_Task_SendMsg(&time_msg);
    p_rsp->command = CMD_SET_SUN_IMITATION_MODE;
    p_rsp->result  = SUCCESS;
}

//-------------------------------------------------------------------------------------------------

static void ctrl_GetStatus(ctrl_req_p p_req, ctrl_rsp_p p_rsp, uint16_t * p_rsp_len)
{
    enum
    {
        MAX_DATETIME_LEN = sizeof(p_rsp->status.datetime.data),
    };
    climate_measurements_t meas     = {0};
    led_color_t            color    = {0};
    time_t                 now      = 0;
    struct tm              datetime = {0};

    time(&now);
    localtime_r(&now, &datetime);

    p_rsp->command = CMD_GET_STATUS;
    p_rsp->result  = SUCCESS;
    if (FW_TRUE == Time_Task_IsInSunImitationMode())
    {
        p_rsp->status.mode = MODE_SUN_IMITATION;
    }
    else
    {
        p_rsp->status.mode = MODE_COLOR;
    }
    LED_Task_GetCurrentColor(&color);
    p_rsp->status.color.r     = color.r;
    p_rsp->status.color.g     = color.g;
    p_rsp->status.color.b     = color.b;
    p_rsp->status.ultraviolet = LED_Task_GetCurrentUltraViolet();
    p_rsp->status.white       = LED_Task_GetCurrentWhite();
    p_rsp->status.fito        = LED_Task_GetCurrentFito();
    p_rsp->status.fan         = Climate_Task_GetFanSpeed();
    p_rsp->status.humidifier  = Climate_Task_IsHumidifierOn();
    Climate_Task_GetMeasurements(&meas);
    p_rsp->status.pressure        = meas.pressure;
    p_rsp->status.temperature     = meas.temperature;
    p_rsp->status.humidity        = meas.humidity;
    p_rsp->status.datetime.length = strftime
                                    (
                                        p_rsp->status.datetime.data,
                                        MAX_DATETIME_LEN,
                                        "%c",
                                        &datetime
                                    );

    *p_rsp_len  = OFFSET_OF(ctrl_rsp_p, status.datetime.length);
    *p_rsp_len += (p_rsp->status.datetime.length + sizeof(p_rsp->status.datetime.length));
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetUltraViolet(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    HTTPS_LOGI("The UV received: %d", p_req->value);

    led_message_t led_msg =
    {
        .command     = LED_CMD_UV_INDICATE_BRIGHTNESS,
        .src_color.a = 0,
        .dst_color.a = p_req->value,
        .interval    = 0,
        .duration    = 0
    };
    LED_Task_SendMsg(&led_msg);

    time_message_t time_msg =
    {
        .command = TIME_CMD_SUN_DISABLE,
    };
    Time_Task_SendMsg(&time_msg);

    p_rsp->command = CMD_SET_ULTRAVIOLET;
    p_rsp->result  = SUCCESS;
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetWhite(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    HTTPS_LOGI("The W received: %d", p_req->value);

    led_message_t led_msg =
    {
        .command     = LED_CMD_W_INDICATE_BRIGHTNESS,
        .src_color.a = 0,
        .dst_color.a = p_req->value,
        .interval    = 0,
        .duration    = 0
    };
    LED_Task_SendMsg(&led_msg);

    time_message_t time_msg =
    {
        .command = TIME_CMD_SUN_DISABLE,
    };
    Time_Task_SendMsg(&time_msg);

    p_rsp->command = CMD_SET_WHITE;
    p_rsp->result  = SUCCESS;
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetFito(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    HTTPS_LOGI("The Fito received: %d", p_req->value);

    led_message_t led_msg =
    {
        .command     = LED_CMD_F_INDICATE_BRIGHTNESS,
        .src_color.a = 0,
        .dst_color.a = p_req->value,
        .interval    = 0,
        .duration    = 0
    };
    LED_Task_SendMsg(&led_msg);

    time_message_t time_msg =
    {
        .command = TIME_CMD_SUN_DISABLE,
    };
    Time_Task_SendMsg(&time_msg);

    p_rsp->command = CMD_SET_FITO;
    p_rsp->result  = SUCCESS;
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetFAN(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    HTTPS_LOGI("The FAN received: %d", p_req->value);

    climate_message_t clt_msg = {0};
    clt_msg.command = CLIMATE_CMD_FAN,
    clt_msg.speed   = (fan_speed_t)(p_req->value % (FAN_SPEED_FULL + 1));
    Climate_Task_SendMsg(&clt_msg);

    time_message_t time_msg = {.command = TIME_CMD_SUN_DISABLE};
    Time_Task_SendMsg(&time_msg);

    p_rsp->command = CMD_SET_FAN;
    p_rsp->result  = SUCCESS;
}

//-------------------------------------------------------------------------------------------------

static void ctrl_SetHumidifier(ctrl_req_p p_req, ctrl_rsp_p p_rsp)
{
    HTTPS_LOGI("The Humidifier received: %d", p_req->value);

    climate_message_t clt_msg = {0};
    clt_msg.command = CLIMATE_CMD_HUMIDIFY,
    clt_msg.on      = (bool)(p_req->value & 1);
    Climate_Task_SendMsg(&clt_msg);

    time_message_t time_msg = {.command = TIME_CMD_SUN_DISABLE};
    Time_Task_SendMsg(&time_msg);

    p_rsp->command = CMD_SET_HUMIDIFIER;
    p_rsp->result  = SUCCESS;
}

//-------------------------------------------------------------------------------------------------

static void ctrl_GetDayMeasurements(ctrl_req_p p_req, ctrl_rsp_p p_rsp, uint16_t * p_rsp_len)
{
    HTTPS_LOGI("The Day Measurements request received");

    p_rsp->command = CMD_GET_DAY_MEASUREMENTS;
    p_rsp->result  = SUCCESS;
    Climate_Task_GetDayMeasurements(&p_rsp->day_measmts);

    *p_rsp_len  = OFFSET_OF(ctrl_rsp_p, day_measmts);
    *p_rsp_len += sizeof(ctrl_day_measmts_t);
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
    ctrl_req_p p_req   = (ctrl_req_p)data;
    uint16_t   rsp_len = 2;
    ctrl_rsp_t rsp     =
    {
        .command = CMD_UNKNOWN,
        .result  = ERROR,
    };

    switch (data[0])
    {
        case CMD_GET_CONNECTION_PARAMETERS:
            ctrl_GetConnParams(&rsp, &rsp_len);
            break;
        case CMD_SET_CONNECTION_PARAMETERS:
            ctrl_SetConnParams(p_req, &rsp);
            break;
        case CMD_SET_COLOR:
            ctrl_SetColor(p_req, &rsp);
            break;
        case CMD_SET_SUN_IMITATION_MODE:
            ctrl_SetSunImitationMode(p_req, &rsp);
            break;
        case CMD_GET_STATUS:
            ctrl_GetStatus(p_req, &rsp, &rsp_len);
            break;
        case CMD_SET_ULTRAVIOLET:
            ctrl_SetUltraViolet(p_req, &rsp);
            break;
        case CMD_SET_WHITE:
            ctrl_SetWhite(p_req, &rsp);
            break;
        case CMD_SET_FITO:
            ctrl_SetFito(p_req, &rsp);
            break;
        case CMD_SET_FAN:
            ctrl_SetFAN(p_req, &rsp);
            break;
        case CMD_SET_HUMIDIFIER:
            ctrl_SetHumidifier(p_req, &rsp);
            break;
        case CMD_GET_DAY_MEASUREMENTS:
            ctrl_GetDayMeasurements(p_req, &rsp, &rsp_len);
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

    websocket_write(pcb, (uint8_t *)&rsp, rsp_len, WS_BIN_MODE);
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
        (void)xTaskCreatePinnedToCore
        (
            vWebSocket_Task,
            "WebSocket",
            4096,
            (void *)pcb,
            2,
            NULL,
            CORE0
        );
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
    (void)xTaskCreatePinnedToCore(vHTTP_Server_Task, "HTTP Server", 6144, NULL, 2, NULL, CORE0);
}

//-------------------------------------------------------------------------------------------------
