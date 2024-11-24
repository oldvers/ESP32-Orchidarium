#include <string.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mdns.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "types.h"
#include "wifi_task.h"
#include "udp_dns_server.h"
#include "http_server.h"
#include "led_task.h"

//-------------------------------------------------------------------------------------------------

#define WIFI_SSID                    "HomeWLAN"
#define WIFI_PSWD                    "************"
#define WIFI_SITE                    "home.com"
#define WIFI_BOOT_CONFIG_IS_ABSENT   (0xDEADBEEF)
#define WIFI_BOOT_AP_NOT_IN_RANGE    (0xCAFEFACE)
#define WIFI_BOOT_CONNECT_TO_AP      (0xBEDECADE)
#define EVT_WIFI_ST_STARTED          BIT0
#define EVT_WIFI_ST_CONNECTED        BIT1
#define EVT_WIFI_ST_GOT_IP           BIT2
#define EVT_WIFI_ST_DISCONNECTED     BIT3
#define EVT_WIFI_AP_ST_CONNECTED     BIT4
#define EVT_WIFI_AP_ST_DISCONNECTED  BIT5

#define WIFI_LOG  1

#if (1 == WIFI_LOG)
static const char * gTAG = "WiFi";
#    define WIFI_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define WIFI_LOGE(...)  ESP_LOGE(gTAG, __VA_ARGS__)
#    define WIFI_LOGW(...)  ESP_LOGV(gTAG, __VA_ARGS__)
#else
#    define WIFI_LOGI(...)
#    define WIFI_LOGE(...)
#    define WIFI_LOGW(...)
#endif

//-------------------------------------------------------------------------------------------------

typedef struct
{
    wifi_string_t ssid;
    wifi_string_t pswd;
    wifi_string_t site;
    void       (* notify_connected)(uint32_t ip);
    void       (* notify_disconnected)(void);
    bool          valid;
    uint8_t       count;
} wifi_params_t;

//-------------------------------------------------------------------------------------------------

static EventGroupHandle_t           gWiFiEvents = NULL;
static uint32_t                     gIpAddr     = 0;
static wifi_params_t                gWiFiParams = {0};
static unsigned int RTC_NOINIT_ATTR gWiFiBoot;

//-------------------------------------------------------------------------------------------------

#define HNDLR_REG(base,id,handler)                                            \
        do                                                                    \
        {                                                                     \
            esp_err_t result;                                                 \
            result = esp_event_handler_register(base, id, handler, NULL);     \
            ESP_ERROR_CHECK(result);                                          \
        } while (0);

#define HNDLR_UNREG(base,id,handler)                                          \
        do                                                                    \
        {                                                                     \
            esp_err_t result;                                                 \
            result = esp_event_handler_unregister(base, id, handler);         \
            ESP_ERROR_CHECK(result);                                          \
        } while (0);

//-------------------------------------------------------------------------------------------------

static EventBits_t wifi_WaitFor(EventBits_t events, TickType_t timeout)
{
    EventBits_t bits = 0;

    /* Waiting until either specified event is set */
    bits = xEventGroupWaitBits
           (
               gWiFiEvents,
               events,       /* Bits To Wait For */
               pdTRUE,       /* Clear On Exit */
               pdFALSE,      /* Wait For All Bits */
               pdMS_TO_TICKS(timeout)
           );

    return bits;
}

//-------------------------------------------------------------------------------------------------

static void wifi_ST_OnStarted
(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data
)
{
    WIFI_LOGI("ST Started!");
    xEventGroupSetBits(gWiFiEvents, EVT_WIFI_ST_STARTED);
}

//-------------------------------------------------------------------------------------------------

static void wifi_ST_OnConnected
(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data
)
{
    WIFI_LOGI("ST Connected to \"%s\"", gWiFiParams.ssid.data);
    xEventGroupSetBits(gWiFiEvents, EVT_WIFI_ST_CONNECTED);
}

//-------------------------------------------------------------------------------------------------

static void wifi_ST_OnGotIp
(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data
)
{
    ip_event_got_ip_t * event        = (ip_event_got_ip_t *)event_data;
    char                addr_str[16] = {0};

    gIpAddr = event->ip_info.ip.addr;
    inet_ntoa_r(gIpAddr, addr_str, sizeof(addr_str) - 1);
    WIFI_LOGI("ST Got IP : %s", addr_str);

    xEventGroupSetBits(gWiFiEvents, EVT_WIFI_ST_GOT_IP);
}    

//-------------------------------------------------------------------------------------------------

static void wifi_ST_OnDisconnected
(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data
)
{ 
    WIFI_LOGI("ST Disconnected!");
    xEventGroupSetBits(gWiFiEvents, EVT_WIFI_ST_DISCONNECTED);
}

//-------------------------------------------------------------------------------------------------

static void wifi_AP_OnStConnected
(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data
)
{
    wifi_event_ap_staconnected_t * event = (wifi_event_ap_staconnected_t *)event_data;

    WIFI_LOGI("AP - ST Connected! "MACSTR", AID=%d", MAC2STR(event->mac), event->aid);
    xEventGroupSetBits(gWiFiEvents, EVT_WIFI_AP_ST_CONNECTED);
}

//-------------------------------------------------------------------------------------------------

static void wifi_AP_OnStDisconnected
(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data
)
{
    wifi_event_ap_stadisconnected_t * event = (wifi_event_ap_stadisconnected_t *)event_data;

    WIFI_LOGI("AP - ST Disconnected! "MACSTR", AID=%d", MAC2STR(event->mac), event->aid);
    xEventGroupSetBits(gWiFiEvents, EVT_WIFI_AP_ST_DISCONNECTED);
}

//-------------------------------------------------------------------------------------------------

static void wifi_RegisterHandlers(void)
{
    HNDLR_REG
    (
        WIFI_EVENT,
        WIFI_EVENT_STA_START,
        &wifi_ST_OnStarted
    );
    HNDLR_REG
    (
        WIFI_EVENT,
        WIFI_EVENT_STA_CONNECTED,
        &wifi_ST_OnConnected
    );
    HNDLR_REG
    (
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_ST_OnGotIp
    );
    HNDLR_REG
    (
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        &wifi_ST_OnDisconnected
    );
    HNDLR_REG
    (
        WIFI_EVENT,
        WIFI_EVENT_AP_STACONNECTED,
        &wifi_AP_OnStConnected
    );
    HNDLR_REG
    (
        WIFI_EVENT,
        WIFI_EVENT_AP_STADISCONNECTED,
        &wifi_AP_OnStDisconnected
    );
}

//-------------------------------------------------------------------------------------------------

static void wifi_mDNS_Init(void)
{
    /* Initialize mDNS */
    ESP_ERROR_CHECK(mdns_init());
    /* Set mDNS hostname (required if you want to advertise services) */
    ESP_ERROR_CHECK(mdns_hostname_set(gWiFiParams.site.data));
    WIFI_LOGI("mDNS hostname set to: [%s]", gWiFiParams.site.data);
    /* Set default mDNS instance name */
    ESP_ERROR_CHECK(mdns_instance_name_set(gWiFiParams.site.data));
    /* Structure with TXT records */
    mdns_txt_item_t txt[] =
    {
        {"board", "esp32"}
    };
    enum
    {
        txtSize = (sizeof(txt)/sizeof(txt[0])),
    };
    /* Initialize service */
    ESP_ERROR_CHECK(mdns_service_add(gWiFiParams.site.data, "_http", "_tcp", 80, txt, txtSize));
}

//-------------------------------------------------------------------------------------------------

static void wifi_Start(void)
{
    esp_netif_t *       p_esp_netif  = NULL;
    esp_netif_ip_info_t ip_info      = {0};
    wifi_config_t       wifi_config  = {0};
    uint8_t             mac[6]       = {0};
    char                ap_ssid[32]  = {0};
    int                 length       = 0;
    char                addr_str[16] = {0};
    led_message_t       led_msg      = {0};

    /* Prepare the events loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Prepare the default configuration for WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    /* Register event handler */
    wifi_RegisterHandlers();

    /* Prepare the WiFi parameters. Temporary in RAM */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* Set the command for indication */
    led_msg.command = LED_CMD_RGB_INDICATE_FADE; 

    /* Station mode */
    if (WIFI_BOOT_CONNECT_TO_AP == gWiFiBoot)
    {
        /* Indication - Blue Fade (wait for connection) */
        led_msg.dst.color.r = 0;
        led_msg.dst.color.b = 255;
        LED_Task_SendMsg(&led_msg);

        /* Create the default station */
        p_esp_netif = esp_netif_create_default_wifi_sta();

        memcpy(wifi_config.sta.ssid, gWiFiParams.ssid.data, gWiFiParams.ssid.length);
        memcpy(wifi_config.sta.password, gWiFiParams.pswd.data, gWiFiParams.pswd.length);

        gWiFiParams.notify_connected    = NULL;
        gWiFiParams.notify_disconnected = NULL;
        gWiFiParams.count               = 7;

        wifi_mDNS_Init();

        WIFI_LOGI("ST Starting...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        (void)wifi_WaitFor(EVT_WIFI_ST_STARTED, portMAX_DELAY);
    }
    else
    /* Access Point mode */
    {
        /* Indication - Red Fade (wait for connection) */
        led_msg.dst.color.r = 255;
        led_msg.dst.color.b = 0;
        LED_Task_SendMsg(&led_msg);

        /* Create the default access point */
        p_esp_netif = esp_netif_create_default_wifi_ap();

        ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
        length = sprintf
                 (
                     ap_ssid,
                     "WIFI_%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
                 );
        memcpy(wifi_config.ap.ssid, ap_ssid, length);
        wifi_config.ap.ssid_len = length;
        strcpy((char *)wifi_config.ap.password, "0123456789");
        wifi_config.ap.max_connection = 1;
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

        gWiFiParams.notify_connected    = UDP_DNS_NotifyWiFiIsConnected;
        gWiFiParams.notify_disconnected = UDP_DNS_NotifyWiFiIsDisconnected;
        gWiFiParams.count               = 7;

        WIFI_LOGI("AP \"%s\" Starting...", ap_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_ERROR_CHECK(esp_netif_get_ip_info(p_esp_netif, &ip_info));
        gIpAddr = ip_info.ip.addr;
        inet_ntoa_r(ip_info.ip, addr_str, sizeof(addr_str) - 1);
        WIFI_LOGI("AP IP : %s", addr_str);
    }
}

//-------------------------------------------------------------------------------------------------

static FW_BOOLEAN wifi_Connect(void)
{
    EventBits_t   events  = 0;
    FW_BOOLEAN    result  = FW_FALSE;
    led_message_t led_msg = {0};

    /* Station mode */
    if (WIFI_BOOT_CONNECT_TO_AP == gWiFiBoot)
    {
        /* Connect */
        WIFI_LOGI("Connecting to \"%s\"...", gWiFiParams.ssid.data);
        ESP_ERROR_CHECK(esp_wifi_connect());
    
        /* Wait for connection */
        events = wifi_WaitFor(EVT_WIFI_ST_DISCONNECTED | EVT_WIFI_ST_GOT_IP, 15000);
        if (0 != (events & EVT_WIFI_ST_GOT_IP))
        {
            WIFI_LOGI("Connected successfuly");

            /* Indication - Rainbow Rotation (connected) */
            led_msg.command = LED_CMD_RGB_INDICATE_RAINBOW_CIRCULATION;
            LED_Task_SendMsg(&led_msg);

            result = FW_TRUE;
        }
        else if (0 != (events & EVT_WIFI_ST_DISCONNECTED))
        {
            WIFI_LOGE("Reconnection needed");
        }
        else
        {
            WIFI_LOGE("Something wrong! Reconnection needed");
        }
    }
    else
    /* Access Point mode */
    {
        /* Wait for connection */
        events = wifi_WaitFor(EVT_WIFI_AP_ST_CONNECTED, 15000);
        if (0 != (events & EVT_WIFI_AP_ST_CONNECTED))
        {
            WIFI_LOGI("Connected successfuly");

            /* Indication - Running R-G-B (connected) */
            led_msg.command = LED_CMD_RGB_INDICATE_RGB_CIRCULATION;
            LED_Task_SendMsg(&led_msg);

            result = FW_TRUE;
        }
        else
        {
            WIFI_LOGE("Something wrong! Reconnection needed");
        }
    }

    return result;
}

//-------------------------------------------------------------------------------------------------

static void wifi_WaitForDisconnect(void)
{
    led_message_t led_msg = {0};

    /* Set the command for indication */
    led_msg.command = LED_CMD_RGB_INDICATE_FADE;

    /* Station mode */
    if (WIFI_BOOT_CONNECT_TO_AP == gWiFiBoot)
    {
        (void)wifi_WaitFor(EVT_WIFI_ST_DISCONNECTED, portMAX_DELAY);

        /* Indication - Blue Fade (wait for connection) */
        led_msg.dst.color.r = 0;
        led_msg.dst.color.b = 255;
        LED_Task_SendMsg(&led_msg);
    }
    else
    /* Access Point mode */
    {
        (void)wifi_WaitFor(EVT_WIFI_AP_ST_DISCONNECTED, portMAX_DELAY);

        /* Indication - Red Fade (wait for connection) */
        led_msg.dst.color.r = 255;
        led_msg.dst.color.b = 0;
        LED_Task_SendMsg(&led_msg);
    }
}

//-------------------------------------------------------------------------------------------------

static bool wifi_LoadParam(nvs_handle h_nvs, char * p_name, wifi_string_t * p_str)
{
    char      param[WIFI_STRING_MAX_LEN] = {0};
    size_t    length                     = sizeof(param);
    esp_err_t status                     = ESP_OK;

    status = nvs_get_str(h_nvs, p_name, param, &length);
    if (ESP_OK != status) return false;
    if (WIFI_STRING_MAX_LEN < length) return false;
    if (0 == length) return false;
    memcpy(p_str->data, param, length);
    p_str->length = length;

    return true;
}

//-------------------------------------------------------------------------------------------------

static bool wifi_LoadParams(wifi_params_t * p_params)
{
    nvs_handle    h_nvs                      = 0;
    esp_err_t     status                     = ESP_OK;
    bool          result                     = false;

    status = nvs_flash_init();
    ESP_ERROR_CHECK(status);

    status = nvs_open("wifi", NVS_READONLY, &h_nvs);
    if (ESP_OK == status)
    {
        result = true;
        do
        {
            result &= wifi_LoadParam(h_nvs, "ssid", &p_params->ssid);
            result &= wifi_LoadParam(h_nvs, "pswd", &p_params->pswd);
            result &= wifi_LoadParam(h_nvs, "site", &p_params->site);
        }
        while (0);
    }

    if (true == result)
    {
        p_params->valid = true;

        WIFI_LOGI
        (
            "Restored params:\n  - SSID:%d:%s\n  - PSWD:%d:%s\n  - SITE:%d:%s",
            p_params->ssid.length, p_params->ssid.data,
            p_params->pswd.length, p_params->pswd.data,
            p_params->site.length, p_params->site.data
        );
    }
    else
    {
        WIFI_LOGI("No stored params");
    }

    nvs_close(h_nvs);

    return result;
}

//-------------------------------------------------------------------------------------------------

//static void wifi_SaveParams(wifi_params_t * p_params)
//{
//    nvs_handle h_nvs  = 0;
//    esp_err_t  status = ESP_OK;
//
//    status = nvs_flash_init();
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_open("wifi", NVS_READWRITE, &h_nvs);
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_set_str(h_nvs, "ssid", p_params->ssid.data);
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_set_str(h_nvs, "pswd", p_params->pswd.data);
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_set_str(h_nvs, "site", p_params->site.data);
//    ESP_ERROR_CHECK(status);
//
//    ESP_LOGI
//    (
//        TAG, "Stored params:\n  - SSID:%d:%s\n  - PSWD:%d:%s\n  - SITE:%d:%s",
//        p_params->ssid.length, p_params->ssid.data,
//        p_params->pswd.length, p_params->pswd.data,
//        p_params->site.length, p_params->site.data
//    );
//
//    nvs_close(h_nvs);
//}

//-------------------------------------------------------------------------------------------------

//static void wifi_ClearParams(void)
//{
//    nvs_handle h_nvs  = 0;
//    esp_err_t  status = ESP_OK;
//
//    status = nvs_flash_init();
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_open("wifi", NVS_READWRITE, &h_nvs);
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_erase_key(h_nvs, "ssid");
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_erase_key(h_nvs, "pswd");
//    ESP_ERROR_CHECK(status);
//
//    status = nvs_erase_key(h_nvs, "site");
//    ESP_ERROR_CHECK(status);
//
//    ESP_LOGI(TAG, "Cleared params");
//
//    nvs_close(h_nvs);
//}

//-------------------------------------------------------------------------------------------------

static void wifi_NotifyIsConnected(uint32_t ip)
{
    if (NULL != gWiFiParams.notify_connected)
    {
        gWiFiParams.notify_connected(ip);
    }
}

//-------------------------------------------------------------------------------------------------

static void wifi_NotifyIsDisconnected(void)
{
    if (NULL != gWiFiParams.notify_disconnected)
    {
        gWiFiParams.notify_disconnected();
    }
}

//-------------------------------------------------------------------------------------------------

static void wifi_CheckIfRestartNeeded(void)
{
    gWiFiParams.count--;
    if (0 == gWiFiParams.count)
    {
        if (WIFI_BOOT_CONNECT_TO_AP == gWiFiBoot)
        {
            WIFI_LOGI("AP is not in range!");
            gWiFiBoot = WIFI_BOOT_AP_NOT_IN_RANGE;
        }
        else
        {
            WIFI_LOGI("Try to connect to AP again!");
            gWiFiBoot = WIFI_BOOT_CONNECT_TO_AP;
        }
        WIFI_LOGI("Boot = 0x%08X", gWiFiBoot);
        WIFI_LOGI("Restart");
        esp_restart();
    }
}

//-------------------------------------------------------------------------------------------------

static void vWiFi_Task(void * pvParams)
{
    wifi_Start();

    while (FW_TRUE)
    {
        if (FW_TRUE == wifi_Connect())
        {
            wifi_NotifyIsConnected(gIpAddr);
            wifi_WaitForDisconnect();
        }

        wifi_CheckIfRestartNeeded();

        wifi_NotifyIsDisconnected();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

//-------------------------------------------------------------------------------------------------

bool WiFi_SaveParams(wifi_string_p p_ssid, wifi_string_p p_pswd, wifi_string_p p_site)
{
    nvs_handle h_nvs  = 0;
    esp_err_t  status = ESP_OK;

    status = nvs_flash_init();
    ESP_ERROR_CHECK(status);

    status = nvs_open("wifi", NVS_READWRITE, &h_nvs);
    ESP_ERROR_CHECK(status);

    status = nvs_set_str(h_nvs, "ssid", p_ssid->data);
    ESP_ERROR_CHECK(status);

    status = nvs_set_str(h_nvs, "pswd", p_pswd->data);
    ESP_ERROR_CHECK(status);

    status = nvs_set_str(h_nvs, "site", p_site->data);
    ESP_ERROR_CHECK(status);

    WIFI_LOGI
    (
        "Stored params:\n  - SSID:%d:%s\n  - PSWD:%d:%s\n  - SITE:%d:%s",
        p_ssid->length, p_ssid->data,
        p_pswd->length, p_pswd->data,
        p_site->length, p_site->data
    );

    nvs_close(h_nvs);

    /* Indicate that the MCU needs to be restarted */
    gWiFiParams.count = 1;
    gWiFiBoot         = WIFI_BOOT_AP_NOT_IN_RANGE;
    WIFI_LOGI("Boot = 0x%08X", gWiFiBoot);

    return true;
}

//-------------------------------------------------------------------------------------------------

bool WiFi_GetParams(wifi_string_p p_ssid, wifi_string_p p_pswd, wifi_string_p p_site)
{
    if (true == gWiFiParams.valid)
    {
        memcpy(p_ssid, &gWiFiParams.ssid, sizeof(wifi_string_t));
        memcpy(p_pswd, &gWiFiParams.pswd, sizeof(wifi_string_t));
        memcpy(p_site, &gWiFiParams.site, sizeof(wifi_string_t));

        WIFI_LOGI
        (
            "Get params:\n  - SSID:%d:%s\n  - PSWD:%d:%s\n  - SITE:%d:%s",
            p_ssid->length, p_ssid->data,
            p_pswd->length, p_pswd->data,
            p_site->length, p_site->data
        );
    }

    return gWiFiParams.valid;
}

//-------------------------------------------------------------------------------------------------

void WiFi_Task_Init(void)
{
    /* Load WiFi parameters */
    WIFI_LOGI("Boot = 0x%08X", gWiFiBoot);
    if (false == wifi_LoadParams(&gWiFiParams))
    {
        gWiFiBoot = WIFI_BOOT_CONFIG_IS_ABSENT;
    }
    if ((WIFI_BOOT_CONFIG_IS_ABSENT == gWiFiBoot) || (WIFI_BOOT_AP_NOT_IN_RANGE == gWiFiBoot))
    {
        //wifi_ClearParams();
        UDP_DNS_Task_Init();
    }
    else if (WIFI_BOOT_CONNECT_TO_AP == gWiFiBoot)
    {
        //wifi_ClearParams();
    }
    else
    {
        gWiFiBoot = WIFI_BOOT_CONNECT_TO_AP;
    }
    WIFI_LOGI("Config = %d", (WIFI_BOOT_CONNECT_TO_AP != gWiFiBoot));
    HTTP_Server_Init((WIFI_BOOT_CONNECT_TO_AP != gWiFiBoot));

    /* Create the events group for WiFi task */
    gWiFiEvents = xEventGroupCreate();

    (void)xTaskCreatePinnedToCore(vWiFi_Task, "WiFi", 8192, NULL, 5, NULL, CORE0);
}

//-------------------------------------------------------------------------------------------------
