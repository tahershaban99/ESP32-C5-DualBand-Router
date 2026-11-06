#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"
#include "router_globals.h"

extern esp_netif_t *ap_netif;
extern esp_netif_t *sta_netif;

void wifi_manager_init(void);
void wifi_manager_refresh_clients(void);
void wifi_manager_deauth_station(uint16_t aid);
void wifi_manager_advertise_uplink_dns(void);
void wifi_manager_send_arp_probe(uint32_t ip);
void wifi_manager_invalidate_arp_entry(uint32_t ip);
void wifi_manager_record_aged_out(uint32_t ip, const uint8_t mac[6]);
bool wifi_manager_is_recent_aged_out(uint32_t ip, const uint8_t mac[6], uint32_t now_s);
void wifi_manager_clear_aged_out_ip(uint32_t ip);
const char *wifi_manager_get_current_band_str(void);
uint8_t wifi_manager_get_current_band(void);

