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

static esp_netif_t *g_esp_netif_rndis = NULL;

static err_t usbh_rndis_linkoutput(esp_netif_t *netif, struct pbuf *p)
{
    int ret;
    (void)netif;
    usbh_lwip_eth_output_common(p, usbh_rndis_get_eth_txbuf());
    ret = usbh_rndis_eth_output(p->tot_len);
    return (ret < 0) ? ERR_BUF : ERR_OK;
}

void usbh_rndis_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(g_esp_netif_rndis, buf, buflen);
}

static esp_err_t usb_rndis_transmit(void *h, void *buffer, size_t len)
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
    err_t ret = usbh_rndis_linkoutput(g_esp_netif_rndis, p);
    pbuf_free(p);
    return (ret == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void rndis_l2_free(void *h, void *buffer)
{
    return;
}

void usbh_rndis_run(struct usbh_rndis *rndis_class)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = IP_EVENT_ETH_LOST_IP,
        .if_key = "usbh_rndis",
        .if_desc = "usb rndis config device",
        .route_prio = 10,
    };

    void *rndis_dev = (void *)rndis_class;
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = rndis_dev,
        .transmit = usb_rndis_transmit,
        .driver_free_rx_buffer = rndis_l2_free,
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    g_esp_netif_rndis = esp_netif_new(&cfg);
    if (g_esp_netif_rndis == NULL) {
        USB_LOG_ERR("Failed to create esp_netif instance for RNDIS");
        return;
    }

    if (esp_netif_set_mac(g_esp_netif_rndis, rndis_class->mac) != ESP_OK) {
        USB_LOG_ERR("Failed to set MAC address for RNDIS");
    }

    usb_osal_thread_create("usbh_rndis_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rndis_rx_thread, NULL);
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &g_esp_netif_rndis, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_start(g_esp_netif_rndis, 0, 0, 0);
#if LWIP_DHCP
    esp_netif_dhcpc_start(g_esp_netif_rndis);
#endif
}

void usbh_rndis_stop(struct usbh_rndis *rndis_class)
{
    if (!g_esp_netif_rndis) {
        USB_LOGW("RNDIS", "ESP-NETIF is already NULL, nothing to stop.");
        return;
    }
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &g_esp_netif_rndis, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_disconnected(g_esp_netif_rndis, NULL, 0, NULL);
    esp_netif_destroy(g_esp_netif_rndis);
    g_esp_netif_rndis = NULL;
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_CDC_NCM
#include "usbh_cdc_ncm.h"

static esp_netif_t *g_esp_netif_ncm = NULL;

static err_t usbh_cdc_ncm_linkoutput(esp_netif_t *netif, struct pbuf *p)
{
    int ret;
    (void)netif;
    usbh_lwip_eth_output_common(p, usbh_cdc_ncm_get_eth_txbuf());
    ret = usbh_cdc_ncm_eth_output(p->tot_len);
    return (ret < 0) ? ERR_BUF : ERR_OK;
}

void usbh_cdc_ncm_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(g_esp_netif_ncm, buf, buflen);
}

static esp_err_t usb_cdc_ncm_transmit(void *h, void *buffer, size_t len)
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
    err_t ret = usbh_cdc_ncm_linkoutput(g_esp_netif_ncm, p);
    pbuf_free(p);
    return (ret == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void ncm_l2_free(void *h, void *buffer)
{
    return;
}

void usbh_cdc_ncm_run(struct usbh_cdc_ncm *cdc_ncm_class)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = IP_EVENT_ETH_LOST_IP,
        .if_key = "usbh_cdc_ncm",
        .if_desc = "usb cdc ncm config device",
        .route_prio = 10,
    };

    void *ncm_dev = (void *)cdc_ncm_class;
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = ncm_dev,
        .transmit = usb_cdc_ncm_transmit,
        .driver_free_rx_buffer = ncm_l2_free,
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    g_esp_netif_ncm = esp_netif_new(&cfg);
    if (g_esp_netif_ncm == NULL) {
        USB_LOG_ERR("Failed to create esp_netif instance for CDC_NCM");
        return;
    }

    if (esp_netif_set_mac(g_esp_netif_ncm, cdc_ncm_class->mac) != ESP_OK) {
        USB_LOG_ERR("Failed to set MAC address for CDC_NCM");
    }

    usb_osal_thread_create("usbh_cdc_ncm_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_cdc_ncm_rx_thread, NULL);
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &g_esp_netif_ncm, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_start(g_esp_netif_ncm, 0, 0, 0);
#if LWIP_DHCP
    esp_netif_dhcpc_start(g_esp_netif_ncm);
#endif
}

void usbh_cdc_ncm_stop(struct usbh_cdc_ncm *cdc_ncm_class)
{
    if (!g_esp_netif_ncm) {
        USB_LOGW("CDC_NCM", "ESP-NETIF is already NULL, nothing to stop.");
        return;
    }
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &g_esp_netif_ncm, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_disconnected(g_esp_netif_ncm, NULL, 0, NULL);
    esp_netif_destroy(g_esp_netif_ncm);
    g_esp_netif_ncm = NULL;
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_ASIX
#include "usbh_asix.h"

static esp_netif_t *g_esp_netif_asix = NULL;

static err_t usbh_asix_linkoutput(esp_netif_t *netif, struct pbuf *p)
{
    int ret;
    (void)netif;
    usbh_lwip_eth_output_common(p, usbh_asix_get_eth_txbuf());
    ret = usbh_asix_eth_output(p->tot_len);
    return (ret < 0) ? ERR_BUF : ERR_OK;
}

void usbh_asix_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(g_esp_netif_asix, buf, buflen);
}

static esp_err_t usb_asix_transmit(void *h, void *buffer, size_t len)
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
    err_t ret = usbh_asix_linkoutput(g_esp_netif_asix, p);
    pbuf_free(p);
    return (ret == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void asix_l2_free(void *h, void *buffer)
{
    return;
}

void usbh_asix_run(struct usbh_asix *asix_class)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = IP_EVENT_ETH_LOST_IP,
        .if_key = "usbh_asix",
        .if_desc = "usb asix config device",
        .route_prio = 10,
    };

    void *asix_dev = (void *)asix_class;
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = asix_dev,
        .transmit = usb_asix_transmit,
        .driver_free_rx_buffer = asix_l2_free,
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    g_esp_netif_asix = esp_netif_new(&cfg);
    if (g_esp_netif_asix == NULL) {
        USB_LOG_ERR("Failed to create esp_netif instance for ASIX");
        return;
    }

    if (esp_netif_set_mac(g_esp_netif_asix, asix_class->mac) != ESP_OK) {
        USB_LOG_ERR("Failed to set MAC address for ASIX");
    }

    usb_osal_thread_create("usbh_asix_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_asix_rx_thread, NULL);
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &g_esp_netif_asix, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_start(g_esp_netif_asix, 0, 0, 0);
#if LWIP_DHCP
    esp_netif_dhcpc_start(g_esp_netif_asix);
#endif
}

void usbh_asix_stop(struct usbh_asix *asix_class)
{
    if (!g_esp_netif_asix) {
        USB_LOGW("ASIX", "ESP-NETIF is already NULL, nothing to stop.");
        return;
    }
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &g_esp_netif_asix, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_disconnected(g_esp_netif_asix, NULL, 0, NULL);
    esp_netif_destroy(g_esp_netif_asix);
    g_esp_netif_asix = NULL;
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_RTL8152
#include "usbh_rtl8152.h"

static esp_netif_t *g_esp_netif_rtl8152 = NULL;

static err_t usbh_rtl8152_linkoutput(esp_netif_t *netif, struct pbuf *p)
{
    int ret;
    (void)netif;
    usbh_lwip_eth_output_common(p, usbh_rtl8152_get_eth_txbuf());
    ret = usbh_rtl8152_eth_output(p->tot_len);
    return (ret < 0) ? ERR_BUF : ERR_OK;
}

void usbh_rtl8152_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(g_esp_netif_rtl8152, buf, buflen);
}

static esp_err_t usb_rtl8152_transmit(void *h, void *buffer, size_t len)
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
    err_t ret = usbh_rtl8152_linkoutput(g_esp_netif_rtl8152, p);
    pbuf_free(p);
    return (ret == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void rtl8152_l2_free(void *h, void *buffer)
{
    return;
}

void usbh_rtl8152_run(struct usbh_rtl8152 *rtl8152_class)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = IP_EVENT_ETH_LOST_IP,
        .if_key = "usbh_rtl8152",
        .if_desc = "usb rtl8152 config device",
        .route_prio = 10,
    };

    void *rtl8152_dev = (void *)rtl8152_class;
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = rtl8152_dev,
        .transmit = usb_rtl8152_transmit,
        .driver_free_rx_buffer = rtl8152_l2_free,
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    g_esp_netif_rtl8152 = esp_netif_new(&cfg);
    if (g_esp_netif_rtl8152 == NULL) {
        USB_LOG_ERR("Failed to create esp_netif instance for RTL8152");
        return;
    }

    if (esp_netif_set_mac(g_esp_netif_rtl8152, rtl8152_class->mac) != ESP_OK) {
        USB_LOG_ERR("Failed to set MAC address for RTL8152");
    }

    usb_osal_thread_create("usbh_rtl8152_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rtl8152_rx_thread, NULL);
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &g_esp_netif_rtl8152, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_start(g_esp_netif_rtl8152, 0, 0, 0);
#if LWIP_DHCP
    esp_netif_dhcpc_start(g_esp_netif_rtl8152);
#endif
}

void usbh_rtl8152_stop(struct usbh_rtl8152 *rtl8152_class)
{
    if (!g_esp_netif_rtl8152) {
        USB_LOG_WRN("RTL8152", "ESP-NETIF is already NULL, nothing to stop.");
        return;
    }
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &g_esp_netif_rtl8152, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_disconnected(g_esp_netif_rtl8152, NULL, 0, NULL);
    esp_netif_destroy(g_esp_netif_rtl8152);
    g_esp_netif_rtl8152 = NULL;
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_BL616
#include "usbh_bl616.h"

static esp_netif_t *g_esp_netif_bl616 = NULL;

static err_t usbh_bl616_linkoutput(esp_netif_t *netif, struct pbuf *p)
{
    int ret;
    (void)netif;
    usbh_lwip_eth_output_common(p, usbh_bl616_get_eth_txbuf());
    ret = usbh_bl616_eth_output(p->tot_len);
    return (ret < 0) ? ERR_BUF : ERR_OK;
}

void usbh_bl616_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_lwip_eth_input_common(g_esp_netif_bl616, buf, buflen);
}

static esp_err_t usb_bl616_transmit(void *h, void *buffer, size_t len)
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
    err_t ret = usbh_bl616_linkoutput(g_esp_netif_bl616, p);
    pbuf_free(p);
    return (ret == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void bl616_l2_free(void *h, void *buffer)
{
    return;
}

void usbh_bl616_run(struct usbh_bl616 *bl616_class)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = IP_EVENT_ETH_LOST_IP,
        .if_key = "usbh_bl616",
        .if_desc = "usb bl616 config device",
        .route_prio = 10,
    };

    void *bl616_dev = (void *)bl616_class;
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = bl616_dev,
        .transmit = usb_bl616_transmit,
        .driver_free_rx_buffer = bl616_l2_free,
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    g_esp_netif_bl616 = esp_netif_new(&cfg);
    if (g_esp_netif_bl616 == NULL) {
        USB_LOG_ERR("Failed to create esp_netif instance for BL616");
        return;
    }

    if (esp_netif_set_mac(g_esp_netif_bl616, bl616_class->sta_mac) != ESP_OK) {
        USB_LOG_ERR("Failed to set MAC address for BL616");
    }

    usb_osal_thread_create("usbh_bl616", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_bl616_rx_thread, NULL);
#if LWIP_DHCP
    esp_netif_dhcpc_start(g_esp_netif_bl616);
#endif
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &g_esp_netif_bl616, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_start(g_esp_netif_bl616, 0, 0, 0);
}

void usbh_bl616_stop(struct usbh_bl616 *bl616_class)
{
    if (!g_esp_netif_bl616) {
        USB_LOGW("BL616", "ESP-NETIF is already NULL, nothing to stop.");
        return;
    }
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &g_esp_netif_bl616, sizeof(esp_netif_t *), portMAX_DELAY);
    esp_netif_action_disconnected(g_esp_netif_bl616, NULL, 0, NULL);
    esp_netif_destroy(g_esp_netif_bl616);
    g_esp_netif_bl616 = NULL;
}
#endif
