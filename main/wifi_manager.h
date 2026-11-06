#pragma once

#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "router_globals.h"
#include <stdbool.h>
#include <stdint.h>

extern esp_netif_t *ap_netif;
extern esp_netif_t *sta_netif;

void wifi_manager_init(void);
void wifi_manager_refresh_clients(void);
void wifi_manager_deauth_station(uint16_t aid);
void wifi_manager_advertise_uplink_dns(void);
void wifi_manager_send_arp_probe(uint32_t ip);
void wifi_manager_invalidate_arp_entry(uint32_t ip);
void wifi_manager_record_aged_out(uint32_t ip, const uint8_t mac[6]);
bool wifi_manager_is_recent_aged_out(uint32_t ip, const uint8_t mac[6],
                                     uint32_t now_s);
void wifi_manager_clear_aged_out_ip(uint32_t ip);
const char *wifi_manager_get_current_band_str(void);
uint8_t wifi_manager_get_current_band(void);
esp_err_t wifi_manager_scan_networks(wifi_ap_record_t **out_records, uint16_t *out_count);
void wifi_manager_free_scan_records(wifi_ap_record_t *records);
