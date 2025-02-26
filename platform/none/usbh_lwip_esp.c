/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FreeRTOS.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "lwip/netif.h"
#if LWIP_DHCP
#include "lwip/dhcp.h"
// #include "lwip/prot/dhcp.h"
#endif

#include "usbh_core.h"

// #define CONFIG_USBHOST_PLATFORM_CDC_ECM
// #define CONFIG_USBHOST_PLATFORM_CDC_RNDIS
// #define CONFIG_USBHOST_PLATFORM_CDC_NCM
// #define CONFIG_USBHOST_PLATFORM_ASIX
// #define CONFIG_USBHOST_PLATFORM_RTL8152
// #define CONFIG_USBHOST_PLATFORM_BL616

void usbh_lwip_eth_output_common(struct pbuf *p, uint8_t *buf)
{
    struct pbuf *q;
    uint8_t *buffer;

    buffer = buf;
    for (q = p; q != NULL; q = q->next) {
        usb_memcpy(buffer, q->payload, q->len);
        buffer += q->len;
    }
}

void usbh_lwip_eth_input_common(esp_netif_t *esp_netif, uint8_t *buf, uint32_t len)
{
    if (esp_netif == NULL) {
        USB_LOG_ERR("esp_netif handle is NULL");
        return;
    }

    esp_err_t err = esp_netif_receive(esp_netif, buf, len, NULL);
    if (err != ESP_OK) {
        USB_LOG_ERR("esp_netif_receive() failed: %s", esp_err_to_name(err));
    }
}

#ifdef CONFIG_USBHOST_PLATFORM_CDC_ECM
#include "usbh_cdc_ecm.h"

static esp_netif_t *g_esp_netif = NULL;

static err_t usbh_cdc_ecm_linkoutput(esp_netif_t *netif, struct pbuf *p)
{
    int ret;
    (void)netif;

    usbh_lwip_eth_output_common(p, usbh_cdc_ecm_get_eth_txbuf());
    ret = usbh_cdc_ecm_eth_output(p->tot_len);
    if (ret < 0) {
        return ERR_BUF;
    }
    return ERR_OK;
}

void usbh_cdc_ecm_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(g_esp_netif, buf, buflen);
}

static esp_err_t usb_cdc_ecm_transmit(void *h,
                                      void *buffer, size_t len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
    if (!p) {
        USB_LOG_ERR("Failed to allocate pbuf");
        return ESP_ERR_NO_MEM;
    }

    if (pbuf_take(p, buffer, len) != ERR_OK) {
        USB_LOG_ERR("pbuf_take failed");
        pbuf_free(p);
        return ESP_FAIL;
    }

    err_t ret = usbh_cdc_ecm_linkoutput(g_esp_netif, p);

    return (ret == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void l2_free(void *h, void *buffer)
{
    return;
}

void usbh_cdc_ecm_set_link_status(struct usbh_cdc_ecm *cdc_ecm_class)
{
    if (cdc_ecm_class->connect_status) {
        esp_netif_action_connected(g_esp_netif, NULL, 0, NULL);
        esp_event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED, &g_esp_netif, sizeof(esp_netif_t *), portMAX_DELAY);

    } else {
        esp_netif_action_disconnected(g_esp_netif, NULL, 0, NULL);
        esp_event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &g_esp_netif, sizeof(esp_netif_t *), portMAX_DELAY);
    }
}

void usbh_cdc_ecm_run(struct usbh_cdc_ecm *cdc_ecm_class)
{
    // 1) Setup a basic IP configuration (here, using all zeros as placeholders)
    esp_netif_ip_info_t ip_info = { 0 };

    // 2) Derive the inherent configuration similar to IDF's default WiFi AP/DHCP settings.
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = IP_EVENT_ETH_LOST_IP,
        .if_key = "usbh_cdc_eth",
        .if_desc = "usb cdc ecm config device",
        .route_prio = 10,
    };

    // 3) Set up the driver configuration.
    // Use the CDC ECM class pointer (or a static singleton) as the driver handle.
    void *cdc_dev = (void *)cdc_ecm_class;
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = cdc_dev,                // Must be non-NULL.
        .transmit = usb_cdc_ecm_transmit, //usb_cdc_ecm_transmit, // Your low-level transmit function.
        .driver_free_rx_buffer = l2_free, // Function to free RX buffers.
    };

    // 4) Combine inherent and driver config into the overall esp_netif configuration.
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH, // Treat as an Ethernet interface.
    };

    // 5) Create the esp_netif instance.
    g_esp_netif = esp_netif_new(&cfg);
    if (g_esp_netif == NULL) {
        USB_LOG_ERR("Failed to create esp_netif instance");
        return;
    }

    // 6) Set the MAC address for the interface.
    // Assumes cdc_ecm_class->mac contains a valid 6-byte MAC address.
    if (esp_netif_set_mac(g_esp_netif, cdc_ecm_class->mac) != ESP_OK) {
        USB_LOG_ERR("Failed to set MAC address");
    }

    usb_osal_thread_create("usbh_cdc_ecm_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_cdc_ecm_rx_thread, NULL);
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &g_esp_netif, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_start(g_esp_netif, 0, 0, 0);
}

void usbh_cdc_ecm_stop(struct usbh_cdc_ecm *cdc_ecm_class)
{
    if (!g_esp_netif) {
        ESP_LOGW("CDC_ECM", "ESP-NETIF is already NULL, nothing to stop.");
        return;
    }
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &g_esp_netif, sizeof(esp_netif_t *), portMAX_DELAY);

    // Bring down the ESP-NETIF interface
    esp_netif_action_disconnected(g_esp_netif, NULL, 0, NULL);

    // Remove ESP-NETIF instance (so it can be reinitialized later)
    esp_netif_destroy(g_esp_netif);
    g_esp_netif = NULL;
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_CDC_RNDIS
#include "usbh_rndis.h"

struct usb_osal_timer *timer_handle;

static void rndis_dev_keepalive_timeout(void *arg)
{
    struct usbh_rndis *rndis_class = (struct usbh_rndis *)arg;
    usbh_rndis_keepalive(rndis_class);
}

void timer_init(struct usbh_rndis *rndis_class)
{
    timer_handle = usb_osal_timer_create("rndis_keepalive", 5000, rndis_dev_keepalive_timeout, rndis_class, true);
    if (NULL != timer_handle) {
        usb_osal_timer_start(timer_handle);
    } else {
        USB_LOG_ERR("timer creation failed! \r\n");
        for (;;) {
            ;
        }
    }
}

struct netif g_rndis_netif;

static err_t usbh_rndis_linkoutput(struct netif *netif, struct pbuf *p)
{
    int ret;
    (void)netif;

    usbh_lwip_eth_output_common(p, usbh_rndis_get_eth_txbuf());
    ret = usbh_rndis_eth_output(p->tot_len);
    if (ret < 0) {
        return ERR_BUF;
    } else {
        return ERR_OK;
    }
}

void usbh_rndis_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(&g_rndis_netif, buf, buflen);
}

static err_t usbh_rndis_if_init(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->output = etharp_output;
    netif->linkoutput = usbh_rndis_linkoutput;
    return ERR_OK;
}

void usbh_rndis_run(struct usbh_rndis *rndis_class)
{
    struct netif *netif = &g_rndis_netif;

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, rndis_class->mac, 6);

    IP4_ADDR(&g_ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&g_netmask, 0, 0, 0, 0);
    IP4_ADDR(&g_gateway, 0, 0, 0, 0);

    netif = netif_add(netif, &g_ipaddr, &g_netmask, &g_gateway, NULL, usbh_rndis_if_init, tcpip_input);
    netif_set_default(netif);
    while (!netif_is_up(netif)) {
    }

    dhcp_handle = usb_osal_timer_create("dhcp", 200, dhcp_timeout, netif, true);
    if (dhcp_handle == NULL) {
        USB_LOG_ERR("timer creation failed! \r\n");
        while (1) {
        }
    }

    usb_osal_thread_create("usbh_rndis_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rndis_rx_thread, NULL);

    //timer_init(rndis_class);

#if LWIP_DHCP
    dhcp_start(netif);
    usb_osal_timer_start(dhcp_handle);
#endif
}

void usbh_rndis_stop(struct usbh_rndis *rndis_class)
{
    struct netif *netif = &g_rndis_netif;
    (void)rndis_class;

#if LWIP_DHCP
    dhcp_stop(netif);
    dhcp_cleanup(netif);
    usb_osal_timer_delete(dhcp_handle);
#endif
    netif_set_down(netif);
    netif_remove(netif);
    // xTimerStop(timer_handle, 0);
    // xTimerDelete(timer_handle, 0);
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_CDC_NCM
#include "usbh_cdc_ncm.h"

struct netif g_cdc_ncm_netif;

static err_t usbh_cdc_ncm_linkoutput(struct netif *netif, struct pbuf *p)
{
    int ret;
    (void)netif;

    usbh_lwip_eth_output_common(p, usbh_cdc_ncm_get_eth_txbuf());
    ret = usbh_cdc_ncm_eth_output(p->tot_len);
    if (ret < 0) {
        return ERR_BUF;
    } else {
        return ERR_OK;
    }
}

void usbh_cdc_ncm_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(&g_cdc_ncm_netif, buf, buflen);
}

static err_t usbh_cdc_ncm_if_init(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->output = etharp_output;
    netif->linkoutput = usbh_cdc_ncm_linkoutput;
    return ERR_OK;
}

void usbh_cdc_ncm_run(struct usbh_cdc_ncm *cdc_ncm_class)
{
    struct netif *netif = &g_cdc_ncm_netif;

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, cdc_ncm_class->mac, 6);

    IP4_ADDR(&g_ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&g_netmask, 0, 0, 0, 0);
    IP4_ADDR(&g_gateway, 0, 0, 0, 0);

    netif = netif_add(netif, &g_ipaddr, &g_netmask, &g_gateway, NULL, usbh_cdc_ncm_if_init, tcpip_input);
    netif_set_default(netif);
    while (!netif_is_up(netif)) {
    }

    dhcp_handle = usb_osal_timer_create("dhcp", 200, dhcp_timeout, netif, true);
    if (dhcp_handle == NULL) {
        USB_LOG_ERR("timer creation failed! \r\n");
        while (1) {
        }
    }

    usb_osal_thread_create("usbh_cdc_ncm_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_cdc_ncm_rx_thread, NULL);
#if LWIP_DHCP
    dhcp_start(netif);
    usb_osal_timer_start(dhcp_handle);
#endif
}

void usbh_cdc_ncm_stop(struct usbh_cdc_ncm *cdc_ncm_class)
{
    struct netif *netif = &g_cdc_ncm_netif;
    (void)cdc_ncm_class;

#if LWIP_DHCP
    dhcp_stop(netif);
    dhcp_cleanup(netif);
    usb_osal_timer_delete(dhcp_handle);
#endif
    netif_set_down(netif);
    netif_remove(netif);
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_ASIX
#include "usbh_asix.h"

struct netif g_asix_netif;

static err_t usbh_asix_linkoutput(struct netif *netif, struct pbuf *p)
{
    int ret;
    (void)netif;

    usbh_lwip_eth_output_common(p, usbh_asix_get_eth_txbuf());
    ret = usbh_asix_eth_output(p->tot_len);
    if (ret < 0) {
        return ERR_BUF;
    } else {
        return ERR_OK;
    }
}

void usbh_asix_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(&g_asix_netif, buf, buflen);
}

static err_t usbh_asix_if_init(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->output = etharp_output;
    netif->linkoutput = usbh_asix_linkoutput;
    return ERR_OK;
}

void usbh_asix_run(struct usbh_asix *asix_class)
{
    struct netif *netif = &g_asix_netif;

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, asix_class->mac, 6);

    IP4_ADDR(&g_ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&g_netmask, 0, 0, 0, 0);
    IP4_ADDR(&g_gateway, 0, 0, 0, 0);

    netif = netif_add(netif, &g_ipaddr, &g_netmask, &g_gateway, NULL, usbh_asix_if_init, tcpip_input);
    netif_set_default(netif);
    while (!netif_is_up(netif)) {
    }

    dhcp_handle = usb_osal_timer_create("dhcp", 200, dhcp_timeout, netif, true);
    if (dhcp_handle == NULL) {
        USB_LOG_ERR("timer creation failed! \r\n");
        while (1) {
        }
    }

    usb_osal_thread_create("usbh_asix_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_asix_rx_thread, NULL);
#if LWIP_DHCP
    dhcp_start(netif);
    usb_osal_timer_start(dhcp_handle);
#endif
}

void usbh_asix_stop(struct usbh_asix *asix_class)
{
    struct netif *netif = &g_asix_netif;
    (void)asix_class;

#if LWIP_DHCP
    dhcp_stop(netif);
    dhcp_cleanup(netif);
    usb_osal_timer_delete(dhcp_handle);
#endif
    netif_set_down(netif);
    netif_remove(netif);
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_RTL8152
#include "usbh_rtl8152.h"

struct netif g_rtl8152_netif;

static err_t usbh_rtl8152_linkoutput(struct netif *netif, struct pbuf *p)
{
    int ret;
    (void)netif;

    usbh_lwip_eth_output_common(p, usbh_rtl8152_get_eth_txbuf());
    ret = usbh_rtl8152_eth_output(p->tot_len);
    if (ret < 0) {
        return ERR_BUF;
    } else {
        return ERR_OK;
    }
}

void usbh_rtl8152_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(&g_rtl8152_netif, buf, buflen);
}

static err_t usbh_rtl8152_if_init(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->output = etharp_output;
    netif->linkoutput = usbh_rtl8152_linkoutput;
    return ERR_OK;
}

void usbh_rtl8152_run(struct usbh_rtl8152 *rtl8152_class)
{
    struct netif *netif = &g_rtl8152_netif;

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, rtl8152_class->mac, 6);

    IP4_ADDR(&g_ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&g_netmask, 0, 0, 0, 0);
    IP4_ADDR(&g_gateway, 0, 0, 0, 0);

    netif = netif_add(netif, &g_ipaddr, &g_netmask, &g_gateway, NULL, usbh_rtl8152_if_init, tcpip_input);
    netif_set_default(netif);
    while (!netif_is_up(netif)) {
    }

    dhcp_handle = usb_osal_timer_create("dhcp", 200, dhcp_timeout, netif, true);
    if (dhcp_handle == NULL) {
        USB_LOG_ERR("timer creation failed! \r\n");
        while (1) {
        }
    }

    usb_osal_thread_create("usbh_rtl8152_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rtl8152_rx_thread, NULL);
#if LWIP_DHCP
    dhcp_start(netif);
    usb_osal_timer_start(dhcp_handle);
#endif
}

void usbh_rtl8152_stop(struct usbh_rtl8152 *rtl8152_class)
{
    struct netif *netif = &g_rtl8152_netif;
    (void)rtl8152_class;

#if LWIP_DHCP
    dhcp_stop(netif);
    dhcp_cleanup(netif);
    usb_osal_timer_delete(dhcp_handle);
#endif
    netif_set_down(netif);
    netif_remove(netif);
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_BL616
#include "usbh_bl616.h"

struct netif g_bl616_netif;
static err_t usbh_bl616_linkoutput(struct netif *netif, struct pbuf *p)
{
    int ret;
    (void)netif;

    usbh_lwip_eth_output_common(p, usbh_bl616_get_eth_txbuf());
    ret = usbh_bl616_eth_output(p->tot_len);
    if (ret < 0) {
        return ERR_BUF;
    } else {
        return ERR_OK;
    }
}

void usbh_bl616_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(&g_bl616_netif, buf, buflen);
}

static err_t usbh_bl616_if_init(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->output = etharp_output;
    netif->linkoutput = usbh_bl616_linkoutput;
    return ERR_OK;
}

void usbh_bl616_sta_connect_callback(void)
{
}

void usbh_bl616_sta_disconnect_callback(void)
{
    struct netif *netif = &g_bl616_netif;

    netif_set_down(netif);
}

void usbh_bl616_sta_update_ip(uint8_t ip4_addr[4], uint8_t ip4_mask[4], uint8_t ip4_gw[4])
{
    struct netif *netif = &g_bl616_netif;

    IP4_ADDR(&netif->ip_addr, ip4_addr[0], ip4_addr[1], ip4_addr[2], ip4_addr[3]);
    IP4_ADDR(&netif->netmask, ip4_mask[0], ip4_mask[1], ip4_mask[2], ip4_mask[3]);
    IP4_ADDR(&netif->gw, ip4_gw[0], ip4_gw[1], ip4_gw[2], ip4_gw[3]);

    netif_set_up(netif);
}

void usbh_bl616_run(struct usbh_bl616 *bl616_class)
{
    struct netif *netif = &g_bl616_netif;

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, bl616_class->sta_mac, 6);

    IP4_ADDR(&g_ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&g_netmask, 0, 0, 0, 0);
    IP4_ADDR(&g_gateway, 0, 0, 0, 0);

    netif = netif_add(netif, &g_ipaddr, &g_netmask, &g_gateway, NULL, usbh_bl616_if_init, tcpip_input);
    netif_set_down(netif);
    netif_set_default(netif);

    dhcp_handle = usb_osal_timer_create("dhcp", 200, dhcp_timeout, netif, true);
    if (dhcp_handle == NULL) {
        USB_LOG_ERR("timer creation failed! \r\n");
        while (1) {
        }
    }
    usb_osal_timer_start(dhcp_handle);

    usb_osal_thread_create("usbh_bl616", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_bl616_rx_thread, NULL);
}

void usbh_bl616_stop(struct usbh_bl616 *bl616_class)
{
    struct netif *netif = &g_bl616_netif;

    (void)bl616_class;

    netif_set_down(netif);
    netif_remove(netif);
}

// #include "shell.h"

// CSH_CMD_EXPORT(wifi_sta_connect, );
// CSH_CMD_EXPORT(wifi_scan, );
#endif