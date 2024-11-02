#ifndef __UDP_DNS_SERVER_H__
#define __UDP_DNS_SERVER_H__

void UDP_DNS_NotifyWiFiIsConnected(uint32_t ip);
void UDP_DNS_NotifyWiFiIsDisconnected(void);
void UDP_DNS_Task_Init(void);

#endif /* __UDP_DNS_SERVER_H__ */
