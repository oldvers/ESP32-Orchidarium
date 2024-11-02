#include <string.h>
#include <sys/param.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#include "types.h"
#include "led_strip.h"
#include "udp_dns_server.h"

//-------------------------------------------------------------------------------------------------

#define PORT                     53
#define EVT_WIFI_CONNECTED       BIT0

/* QR.
 * A one bit field that specifies whether this message is a query or a response.
 */
#define QR_QUERY     0
#define QR_RESPONSE  1

/* OPCODE.
 * A four bit field that specifies kind of query in this message.
 */
#define OPCODE_QUERY   0 /* A standard query. */
#define OPCODE_IQUERY  1 /* An inverse query. */
#define OPCODE_STATUS  2 /* A server status request. */
/* 3-15 reserved for future use. */

/* AA.
 * A one bit, valid in responses, and specifies that the responding
 * name server is an authority for the domain name in question section.
 */
#define AA_NONAUTHORITY  0
#define AA_AUTHORITY     1

/* Maximum domain name octet length without zero terminated char for this server. */
#define DNS_MAX_OCTET_LEN 60

#define DNS_LOG  0

#if (1 == DNS_LOG)
static const char * gTAG = "DNS";
#    define DNS_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define DNS_LOGE(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define DNS_LOGV(...)
#elif (2 == DNS_LOG)
static const char * gTAG = "DNS";
#    define DNS_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define DNS_LOGE(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define DNS_LOGV(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#else
#    define DNS_LOGI(...)
#    define DNS_LOGE(...)
#    define DNS_LOGV(...)
#endif

//-------------------------------------------------------------------------------------------------

/** DNS record types enumeration. */
typedef enum
{
    DNS_TYPE_A     = 0x01,
    DNS_TYPE_NS    = 0x02,
    DNS_TYPE_CNAME = 0x05,
    DNS_TYPE_SOA   = 0x06,
    DNS_TYPE_WKS   = 0x0B,
    DNS_TYPE_PTR   = 0x0C,
    DNS_TYPE_MX    = 0x0F,
    DNS_TYPE_TXT   = 0x10,
    DNS_TYPE_SRV   = 0x21,
} dns_type_t;

typedef struct dns_header
{
    uint16_t id;    /* A 16 bit identifier assigned by the client, big endian. */
    /* Flags L */
    uint8_t  rd     : 1;
    uint8_t  tc     : 1;
    uint8_t  aa     : 1;
    uint8_t  opcode : 4;
    uint8_t  qr     : 1;
    /* Flags H */
    uint8_t  rcode  : 4;
    uint8_t  z      : 3;
    uint8_t  ra     : 1;
    /* All fields should be filled with big endian. */
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

typedef struct dns_packet
{
    dns_header_t header;
    char         data[500];
} dns_packet_t;

/* DNS response header.
 * All fields should be filled with big endian.
 */
typedef struct __attribute__((packed))
{
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t size;
    uint8_t  data[];
} dns_answer_t;

//-------------------------------------------------------------------------------------------------

static const char * gURL[] =
{
    "home.local",
    "home.com",
    "google.com",
    "apple.com",
    "microsoft.com",
    "msftncsi.com",
    "msft",
    "gstatic.com"
};

//-------------------------------------------------------------------------------------------------

static EventGroupHandle_t gDnsEvents       = NULL;
static uint32_t           gIpAddr          = 0;
static uint8_t            gDnsBuffer[1024] = {0};

//-------------------------------------------------------------------------------------------------

#if (2 == DNS_LOG)
#define DNS_LOG_Q_BUF_DECL()      char log[DNS_MAX_OCTET_LEN] = {0}
#define DNS_LOG_Q_BUF()           log
#define DNS_LOG_Q_APPEND(idx,chr) log[idx] = chr
#else
#define DNS_LOG_Q_BUF_DECL()
#define DNS_LOG_Q_BUF()
#define DNS_LOG_Q_APPEND(...)
#endif

static int dns_ParseQuestion
(
    dns_packet_t * p_pkt,
    uint16_t size,
    char * p_name,
    uint16_t * p_type,
    uint16_t * p_class
)
{
    uint16_t i, length, j;
    char *   question               = p_pkt->data;

    DNS_LOG_Q_BUF_DECL();

    if (DNS_MAX_OCTET_LEN < size)
    {
        return 0;
    }

    i = 0;
    length = question[i++];

    do
    {
        for (j = 0; j < length; j++)
        {
            *p_name++ = question[i + j];
            DNS_LOG_Q_APPEND(j, question[i + j]);
        }
        DNS_LOG_Q_APPEND(j, '\0');
        DNS_LOGV("  - Size: %d - Question: %s", length, DNS_LOG_Q_BUF());

        *p_name++ = '.';
        i += length;
        length = question[i++];
    }
    while ((length != 0) && (i < size));
    *--p_name = '\0';

    *p_type = (question[i++] << 8);
    *p_type += question[i++];

    *p_class = (question[i++] << 8);
    *p_class += question[i++];

    return 1;
}

//-------------------------------------------------------------------------------------------------

#if (2 == DNS_LOG)
#define DNS_LOG_R_ID_DECL() uint16_t id = ntohs(p_pkt->header.id)
#define DNS_LOG_R_ID()      id
#else
#define DNS_LOG_R_ID_DECL()
#define DNS_LOG_R_ID()
#endif

static int dns_ParseRequest(uint8_t * p_buf, int16_t size, char * p_name)
{
    dns_packet_t * p_pkt                   = (dns_packet_t *)p_buf;
    uint16_t       qdcount                 = ntohs(p_pkt->header.qdcount);
    uint16_t       type                    = 0;
    uint16_t       class                   = 0;
    int            result                  = 0;

    DNS_LOG_R_ID_DECL();

    /* Parse only single queries. */
    if ((QR_QUERY == p_pkt->header.qr) && (1 == qdcount))
    {
        DNS_LOGV("--- ID: %d - QR: %d - QD Count: %d", DNS_LOG_R_ID(), p_pkt->header.qr, qdcount);
        DNS_LOGV("  - Data Size: %d", (int)(size - sizeof(p_pkt->header)));

        size = (size - sizeof(p_pkt->header));

        dns_ParseQuestion(p_pkt, size, p_name, &type, &class);

        if ((DNS_TYPE_A == type) || (DNS_TYPE_PTR == type))
        {
            DNS_LOGI("  - Name: %s", p_name);

            result = 1;
        }
    }

    return result;
}

//-------------------------------------------------------------------------------------------------

static uint32_t dns_PrepareName(uint8_t * p_buf, const char * p_name)
{
    uint32_t  pos = 1;
    uint8_t * sp  = p_buf;
    uint32_t  len = 0;

    while (*p_name)
    {
        if ('.' == *p_name)
        {
            *sp = len;
            len = 0;
            sp  = &p_buf[pos];
        }
        else
        {
            p_buf[pos] = *p_name;
            if (DNS_MAX_OCTET_LEN <= len)
            {
                break;
            }
            len++;
        }
        pos++;
        p_name++;
    }
    *sp = len;
    p_buf[pos++] = 0x00;

    return pos;
}

//-------------------------------------------------------------------------------------------------

static int dns_PrepareAnswer(uint8_t * p_buf, uint16_t size, uint32_t ip, const char * p_name)
{
    dns_packet_t * p_pkt   = (dns_packet_t *)p_buf;
    uint32_t       pos     = size;
    uint32_t       ip_addr = ip;
    uint32_t       size1   = sizeof(ip_addr);
    uint8_t *      data1   = (uint8_t *)&ip_addr;
    dns_answer_t * p_resp  = NULL;
    uint32_t       datapos = 0;

    /* Always add response with host address data to the end */
    p_pkt->header.ancount = ntohs(1);
    p_pkt->header.nscount = ntohs(0);
    p_pkt->header.arcount = ntohs(0);
    p_pkt->header.qr      = 1;
    p_pkt->header.aa      = 0;
    p_pkt->header.ra      = 1;
    p_pkt->header.rd      = 1;
    p_pkt->header.rcode   = 0;

    /* Prepare the name or link to the name in the question. */
    if (NULL == p_name)
    {
        p_buf[pos++] = 0xC0;
        p_buf[pos++] = sizeof(dns_header_t);
    }
    else
    {
        pos += dns_PrepareName(&p_buf[pos], p_name);
    }

    /* Prepare the response - IP Address. */
    p_resp        = (dns_answer_t *)&p_buf[pos];
    p_resp->type  = ntohs(DNS_TYPE_A);       // A - type
    p_resp->class = ntohs(1);                // IN - class
    p_resp->ttl   = ntohl(5 * 24 * 60 * 60); // TTL

    pos += sizeof(dns_answer_t);
    datapos = pos;
    if (data1)
    {
        while(size1--)
        {
            p_buf[pos] = *data1;
            pos++;
            data1++;
        }
    }

    p_resp->size = ntohs(pos - datapos);
    return pos;
}

//-------------------------------------------------------------------------------------------------

static int dns_ProcessRequest(uint8_t * p_buf, uint16_t size)
{
    char           name[DNS_MAX_OCTET_LEN] = {0};
    char           addr[16]                = {0};
    uint32_t       rip                     = ntohl(gIpAddr);
    int            result                  = 0;
    int            idx                     = 0;

    if (dns_ParseRequest(gDnsBuffer, size, name))
    {
        do
        {
            /* Check the Host Name request */
            inet_ntoa_r(rip, addr, sizeof(addr) - 1);
            DNS_LOGV("  - Reversed IP: %s, %08X", addr, (int)rip);
            if (NULL != strstr(name, addr))
            {
                result = dns_PrepareAnswer(gDnsBuffer, size, gIpAddr, gURL[0]);
                break;
            }

            /* Check the IP address request */
            for (idx = 0; idx < (sizeof(gURL)/sizeof(char *)); idx++)
            {
                if (NULL != strstr(name, gURL[idx]))
                {
                    result = dns_PrepareAnswer(gDnsBuffer, size, gIpAddr, NULL);
                    break;
                }
            }
        }
        while (0);
    }

    return result;
}

//-------------------------------------------------------------------------------------------------

static EventBits_t dns_WaitFor(EventBits_t events, TickType_t timeout)
{
    EventBits_t bits = 0;

    /* Waiting until either specified event is set */
    bits = xEventGroupWaitBits
           (
               gDnsEvents,
               events,       /* Bits To Wait For */
               pdFALSE,      /* Clear On Exit */
               pdFALSE,      /* Wait For All Bits */
               pdMS_TO_TICKS(timeout)
           );

    return bits;
}

//-------------------------------------------------------------------------------------------------

static void vDNS_Task(void * pvParameters)
{
    struct sockaddr_in svrAddr      = {0};
    struct sockaddr_in cltAddr      = {0};
    char               addr_str[16] = {0};
    int                addr_family  = 0;
    int                ip_protocol  = 0;
    struct timeval     timeouts     = {0};
    socklen_t          socklen      = 0;
    int                datalen      = 0;
    uint32_t           addr         = 0;
    int                error        = 0;

    while (FW_TRUE)
    {
        DNS_LOGI("Waiting for WiFi connection...");
        (void)dns_WaitFor(EVT_WIFI_CONNECTED, portMAX_DELAY);

        /* Create the socket */
        svrAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        svrAddr.sin_family = AF_INET;
        svrAddr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(gIpAddr, addr_str, sizeof(addr_str) - 1);
        DNS_LOGI("Creating socket, IP: %s, %08X", addr_str, (int)gIpAddr);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0)
        {
            DNS_LOGE("Unable to create socket: errno %d", errno);
            break;
        }
        DNS_LOGI("Socket created");

        /* Set timeouts */
        timeouts.tv_sec = 1;
        timeouts.tv_usec = 0;
        error = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeouts, sizeof(timeouts));
        if (error < 0)
        {
            DNS_LOGE("Unable to set socket timeouts: errno %d", errno);
            closesocket(sock);
            break;
        }

        error = bind(sock, (struct sockaddr *)&svrAddr, sizeof(svrAddr));
        if (error < 0)
        {
            DNS_LOGE("Socket unable to bind: errno %d", errno);
            break;
        }
        DNS_LOGI("Socket binded");

        while (FW_TRUE)
        {
            socklen = sizeof(cltAddr);
            datalen = recvfrom
                      (
                          sock,
                          gDnsBuffer,
                          sizeof(gDnsBuffer) - 1,
                          0,
                          (struct sockaddr *)&cltAddr,
                          &socklen
                      );
            if (0 < datalen)
            {
                /* Get the sender's ip address as string */
                addr = ((struct sockaddr_in *)&cltAddr)->sin_addr.s_addr;
                inet_ntoa_r(addr, addr_str, sizeof(addr_str) - 1);
                DNS_LOGV("Received %d bytes from %08X:%s", datalen, (int)addr, addr_str);

                if (0 < (datalen = dns_ProcessRequest(gDnsBuffer, datalen)))
                {
                    error = sendto
                            (
                                sock,
                                gDnsBuffer,
                                datalen,
                                0,
                                (struct sockaddr *)&cltAddr,
                                sizeof(cltAddr)
                            );
                    if (error < 0)
                    {
                        DNS_LOGE("Error occured during sending: errno %d", errno);
                        break;
                    }
                    DNS_LOGV("Sent %d bytes", datalen);
                }
            }

            if (0 == (EVT_WIFI_CONNECTED & dns_WaitFor(EVT_WIFI_CONNECTED, 0)))
            {
                DNS_LOGI("WiFi connection is lost...");
                closesocket(sock);
                break;
            }
        }

        if (sock != -1)
        {
            DNS_LOGE("Shutting down socket and restarting...");
            closesocket(sock);
        }
    }
    vTaskDelete(NULL);
}

//-------------------------------------------------------------------------------------------------

void UDP_DNS_NotifyWiFiIsConnected(uint32_t ip)
{
    gIpAddr = ip;

    xEventGroupSetBits(gDnsEvents, EVT_WIFI_CONNECTED);
}

//-------------------------------------------------------------------------------------------------

void UDP_DNS_NotifyWiFiIsDisconnected(void)
{
    xEventGroupClearBits(gDnsEvents, EVT_WIFI_CONNECTED);

    gIpAddr = 0;
}

//-------------------------------------------------------------------------------------------------

void UDP_DNS_Task_Init(void)
{
    /* Create the events group for UDP task */
    gDnsEvents = xEventGroupCreate();

    (void)xTaskCreatePinnedToCore(vDNS_Task, "DNS", 6144, NULL, 5, NULL, CORE0);
}

//-------------------------------------------------------------------------------------------------

