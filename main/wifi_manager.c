#include "wifi_manager.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/lwip_napt.h"
#include "lwip/etharp.h"
#include "meter.h"
#include "router_config.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <time.h>

esp_netif_t *ap_netif = NULL;
esp_netif_t *sta_netif = NULL;

scan_entry_t scan_results[MAX_SCAN_RESULTS];
uint16_t scan_result_count = 0;
bool scan_running = false;
bool scan_ready = false;

static void collect_scan_results(void) {
  uint16_t found = 0;
  esp_err_t num_err = esp_wifi_scan_get_ap_num(&found);
  if (num_err != ESP_OK) found = 0;
  ESP_LOGI(ROUTER_TAG, "Scan completed: APs detected = %u", (unsigned)found);

  uint16_t fetch_count = (found > MAX_SCAN_RESULTS) ? MAX_SCAN_RESULTS : found;
  wifi_ap_record_t *records = (fetch_count > 0) ? calloc(fetch_count, sizeof(wifi_ap_record_t)) : NULL;

  if (records) {
    if (esp_wifi_scan_get_ap_records(&fetch_count, records) == ESP_OK) {
      portENTER_CRITICAL(&data_mux);
      scan_result_count = fetch_count;
      memset(scan_results, 0, sizeof(scan_results));
      for (uint16_t i = 0; i < fetch_count; ++i) {
        memcpy(scan_results[i].ssid, records[i].ssid, sizeof(scan_results[i].ssid) - 1);
        scan_results[i].ssid[sizeof(scan_results[i].ssid) - 1] = 0;
        scan_results[i].rssi = records[i].rssi;
        scan_results[i].secured = (records[i].authmode != WIFI_AUTH_OPEN);
        scan_results[i].band = (records[i].primary >= 36) ? 2 : 1;
        ESP_LOGI(ROUTER_TAG, "  Found AP[%u]: '%s', Ch %u (%s), RSSI %d dBm, %s",
                 (unsigned)i, scan_results[i].ssid, (unsigned)records[i].primary,
                 (scan_results[i].band == 2) ? "5GHz" : "2.4GHz",
                 (int)scan_results[i].rssi, scan_results[i].secured ? "Secured" : "Open");
      }
      scan_running = false;
      scan_ready = true;
      portEXIT_CRITICAL(&data_mux);
    } else {
      ESP_LOGW(ROUTER_TAG, "esp_wifi_scan_get_ap_records failed");
      portENTER_CRITICAL(&data_mux);
      scan_result_count = 0;
      scan_running = false;
      scan_ready = true;
      portEXIT_CRITICAL(&data_mux);
    }
    free(records);
  } else {
    esp_wifi_clear_ap_list();
    portENTER_CRITICAL(&data_mux);
    scan_result_count = 0;
    scan_running = false;
    scan_ready = true;
    portEXIT_CRITICAL(&data_mux);
  }
}

void wifi_manager_advertise_uplink_dns(void) {
  esp_netif_dns_info_t dns = {0};
  if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) return;
  uint8_t offer_dns = 0x02;
  esp_netif_dhcps_stop(ap_netif);
  esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                         ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
  esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
  esp_netif_dhcps_start(ap_netif);
}

static uint32_t sta_fail_count = 0;
static uint32_t next_sta_reconnect_s = 0;
static bool sta_connected_now = false;

static void wifi_manager_init_sntp(void) {
  if (!esp_sntp_enabled()) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    setenv("TZ", "UTC-2", 1);
    tzset();
    ESP_LOGI(ROUTER_TAG, "SNTP time synchronization initialized");
  }
}

static void handle_ap_station_connected(const wifi_event_ap_staconnected_t *event) {
  if (!event) return;
  portENTER_CRITICAL(&data_mux);
  size_t slot = MAX_CLIENTS;
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, event->mac, 6) == 0) {
      slot = i;
      break;
    }
    if (slot == MAX_CLIENTS && active_clients[i].mac[0] == 0 &&
        active_clients[i].mac[1] == 0 && active_clients[i].mac[2] == 0 &&
        active_clients[i].mac[3] == 0 && active_clients[i].mac[4] == 0 &&
        active_clients[i].mac[5] == 0) slot = i;
  }
  if (slot < MAX_CLIENTS) {
    memcpy(active_clients[slot].mac, event->mac, 6);
    active_clients[slot].aid = event->aid;
    active_clients[slot].last_seen_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  }
  meter_find_usage_locked(event->mac, true);
  meter_find_policy_locked(event->mac, true);
  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);
}

static void handle_ap_station_disconnected(const wifi_event_ap_stadisconnected_t *event) {
  if (!event) return;
  portENTER_CRITICAL(&data_mux);
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, event->mac, 6) == 0) {
      active_clients[i].aid = 0;
      break;
    }
  }
  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    collect_scan_results();
    if (has_saved_config) {
      wifi_ap_record_t sta_ap;
      if (esp_wifi_sta_get_ap_info(&sta_ap) != ESP_OK) {
        esp_wifi_connect();
      }
    }
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    // Pause any background STA scan for 8s so the connecting client completes WPA2 + DHCP handshake without channel hopping
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (now_s + 8 > next_sta_reconnect_s) {
      next_sta_reconnect_s = now_s + 8;
    }
    handle_ap_station_connected((const wifi_event_ap_staconnected_t *)data);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    handle_ap_station_disconnected((const wifi_event_ap_stadisconnected_t *)data);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (has_saved_config && !scan_running) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
    uint8_t reason = disc ? disc->reason : 0;
    ESP_LOGW(ROUTER_TAG, "STA disconnected from '%s', reason=%u", router_ssid, (unsigned)reason);
    esp_netif_napt_disable(ap_netif);
    sta_connected_now = false;
    sta_fail_count++;
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t backoff_s = (sta_fail_count <= 2) ? 4 : 15;
    if (now_s + backoff_s > next_sta_reconnect_s) {
      next_sta_reconnect_s = now_s + backoff_s;
    }
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    sta_connected_now = true;
    sta_fail_count = 0;
    next_sta_reconnect_s = 0;
    if (sta_static_enabled) {
      esp_netif_set_default_netif(sta_netif);
      esp_netif_ip_info_t info = {0};
      if (esp_netif_get_ip_info(sta_netif, &info) == ESP_OK && info.ip.addr != 0) {
        esp_netif_dns_info_t dns = {0};
        if (!sta_dns_text[0] || esp_netif_str_to_ip4(sta_dns_text, &dns.ip.u_addr.ip4) != ESP_OK)
          dns.ip.u_addr.ip4 = info.gw;
        esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        wifi_manager_advertise_uplink_dns();
        esp_netif_napt_enable(ap_netif);
        ESP_LOGI(ROUTER_TAG, "Static uplink active at " IPSTR, IP2STR(&info.ip));
      }
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    sta_connected_now = true;
    sta_fail_count = 0;
    next_sta_reconnect_s = 0;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    ESP_LOGI(ROUTER_TAG, "Upstream WAN connected! Got IP: " IPSTR ", Gateway: " IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
    esp_wifi_set_ps(esp_wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    esp_netif_set_default_netif(sta_netif);
    wifi_manager_advertise_uplink_dns();
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    ESP_LOGI(ROUTER_TAG, "NAPT NAT forwarding %s", (err == ESP_OK) ? "ENABLED" : "FAILED");
    wifi_manager_init_sntp();
    uint8_t primary_ch = 1;
    wifi_second_chan_t sec_ch = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary_ch, &sec_ch) == ESP_OK) {
      ESP_LOGI(ROUTER_TAG, "Operating on Channel %u (%s)",
               (unsigned)primary_ch, (primary_ch >= 36) ? "5GHz Band" : "2.4GHz Band");
      if (primary_ch > 11 && primary_ch <= 14) {
        wifi_bandwidth_t cur_bw = WIFI_BW20;
        if (esp_wifi_get_bandwidth(WIFI_IF_AP, &cur_bw) == ESP_OK && cur_bw == WIFI_BW40) {
          ESP_LOGW(ROUTER_TAG, "Upstream AP is on 2.4GHz Channel %u > 11; clamping bandwidth to HT20", (unsigned)primary_ch);
          esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
          esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        }
      }
    }
    ESP_LOGI(ROUTER_TAG, "Upstream connected; NAPT result=%s", esp_err_to_name(err));
  } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
    const ip_event_assigned_ip_to_client_t *event = (const ip_event_assigned_ip_to_client_t *)data;
    if (event) {
      meter_sync_device_on_dhcp(event->mac, event->ip.addr);
      ESP_LOGI(ROUTER_TAG, "Station assigned IP " IPSTR " -> MAC %02X:%02X:%02X:%02X:%02X:%02X",
               IP2STR(&event->ip),
               event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4], event->mac[5]);
    }
  }
}

void wifi_manager_refresh_clients(void) {
  wifi_sta_list_t wifi_list = {0};
  esp_netif_pair_mac_ip_t ip_list[MAX_CLIENTS] = {0};
  active_client_t fresh[MAX_CLIENTS] = {0};
  if (esp_wifi_ap_get_sta_list(&wifi_list) != ESP_OK) return;

  uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

  size_t count = wifi_list.num < MAX_WIFI_STATIONS ? wifi_list.num : MAX_WIFI_STATIONS;
  for (size_t i = 0; i < count; ++i) {
    memcpy(fresh[i].mac, wifi_list.sta[i].mac, 6);
    memcpy(ip_list[i].mac, wifi_list.sta[i].mac, 6);
    fresh[i].rssi = wifi_list.sta[i].rssi;
    fresh[i].last_seen_s = now_s;
    uint8_t prev_aid = 0;
    for (size_t k = 0; k < MAX_CLIENTS; ++k) {
      if (memcmp(active_clients[k].mac, fresh[i].mac, 6) == 0 && active_clients[k].aid != 0 && active_clients[k].aid != 0xFF) {
        prev_aid = active_clients[k].aid;
        break;
      }
    }
    fresh[i].aid = (prev_aid != 0) ? prev_aid : (uint8_t)(i + 1);
  }
  esp_netif_dhcps_get_clients_by_mac(ap_netif, count, ip_list);
  for (size_t i = 0; i < count; ++i) {
    fresh[i].ip = ip_list[i].ip.addr;
  }

  // Query lwIP ARP table for devices using a static IP or connected via Range Extender (Proxy-ARP / Bridge)
  for (size_t a = 0; a < ARP_TABLE_SIZE; ++a) {
    ip4_addr_t *arp_ip = NULL;
    struct netif *arp_netif = NULL;
    struct eth_addr *arp_eth = NULL;
    if (etharp_get_entry(a, &arp_ip, &arp_netif, &arp_eth) && arp_ip && arp_eth && arp_ip->addr != 0) {
      uint32_t ip4 = arp_ip->addr;
      if ((ip4 & 0x00FFFFFF) == 0x0004A8C0 && (ip4 >> 24) != 1 && valid_device_mac(arp_eth->addr)) {
        // 1. Skip if this IP is already assigned in fresh[]
        bool matched = false;
        for (size_t k = 0; k < count; ++k) {
          if (fresh[k].ip == ip4) {
            fresh[k].last_seen_s = now_s;
            matched = true;
            break;
          }
        }
        if (matched) continue;

        // 2. Check if DHCP binding table knows the true client MAC for this IP
        // (Critical for Wi-Fi Range Extenders where ARP eth_addr is the Extender's MAC
        // while DHCP chaddr recorded the actual phone/laptop MAC!)
        uint8_t bound_mac[6] = {0};
        bool has_bound_mac = false;
        portENTER_CRITICAL(&data_mux);
        for (size_t b = 0; b < binding_count; ++b) {
          if (device_bindings[b].bound_ip == ip4 && valid_device_mac(device_bindings[b].mac)) {
            memcpy(bound_mac, device_bindings[b].mac, 6);
            has_bound_mac = true;
            break;
          }
        }
        portEXIT_CRITICAL(&data_mux);

        if (has_bound_mac) {
          for (size_t k = 0; k < count; ++k) {
            if (memcmp(fresh[k].mac, bound_mac, 6) == 0) {
              if (fresh[k].ip == 0) {
                fresh[k].ip = ip4;
                matched = true;
              }
              break;
            }
          }
          if (!matched && count < MAX_CLIENTS) {
            memcpy(fresh[count].mac, bound_mac, 6);
            fresh[count].ip = ip4;
            fresh[count].aid = 0xFF; // Bridged / Range Extender client
            fresh[count].rssi = -55;
            fresh[count].last_seen_s = now_s;
            count++;
            matched = true;
          }
        }

        // 3. If not in DHCP bindings, check if arp_eth->addr matches a direct STA without an IP yet
        if (!matched) {
          for (size_t k = 0; k < count; ++k) {
            if (memcmp(fresh[k].mac, arp_eth->addr, 6) == 0 && fresh[k].ip == 0) {
              fresh[k].ip = ip4;
              matched = true;
              break;
            }
          }
        }

        // 4. New bridged / Range Extender client not in DHCP bindings
        if (!matched && count < MAX_CLIENTS) {
          bool mac_already_used = false;
          for (size_t k = 0; k < count; ++k) {
            if (memcmp(fresh[k].mac, arp_eth->addr, 6) == 0) {
              mac_already_used = true;
              break;
            }
          }
          if (!mac_already_used) {
            memcpy(fresh[count].mac, arp_eth->addr, 6);
          } else {
            // Range Extender Proxy-ARP with static IP: synthesize unique per-IP MAC
            fresh[count].mac[0] = 0x02;
            fresh[count].mac[1] = 0xE0;
            fresh[count].mac[2] = 192;
            fresh[count].mac[3] = 168;
            fresh[count].mac[4] = 4;
            fresh[count].mac[5] = ((const uint8_t *)&ip4)[3];
          }
          fresh[count].ip = ip4;
          fresh[count].aid = 0xFF; // Virtual/bridged client
          fresh[count].rssi = -58;
          fresh[count].last_seen_s = now_s;
          count++;
        }
      }
    }
  }

  portENTER_CRITICAL(&data_mux);
  // Sync latest packet timestamps from fast_device_pool before preserving bridged clients
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (active_clients[k].ip != 0 && fast_device_pool[k].ip == active_clients[k].ip) {
      if (fast_device_pool[k].last_seen_s > active_clients[k].last_seen_s) {
        active_clients[k].last_seen_s = fast_device_pool[k].last_seen_s;
      }
    }
  }

  // Also consume any pending_discovered_ip detected by meter_ip4_canforward
  uint32_t disc_ip = pending_discovered_ip;
  if (disc_ip != 0) {
    pending_discovered_ip = 0;
    uint8_t disc_oct = ((const uint8_t *)&disc_ip)[3];
    if (disc_oct >= 2 && disc_oct <= 254) {
      bool already_in_fresh = false;
      for (size_t m = 0; m < count; ++m) {
        if (fresh[m].ip == disc_ip) {
          fresh[m].last_seen_s = now_s;
          already_in_fresh = true;
          break;
        }
      }
      if (!already_in_fresh && count < MAX_CLIENTS) {
        uint8_t owner_mac[6] = {0};
        for (size_t b = 0; b < binding_count; ++b) {
          if (device_bindings[b].bound_ip == disc_ip && valid_device_mac(device_bindings[b].mac)) {
            memcpy(owner_mac, device_bindings[b].mac, 6);
            break;
          }
        }
        if (!valid_device_mac(owner_mac)) {
          owner_mac[0] = 0x02;
          owner_mac[1] = 0xE0;
          owner_mac[2] = 192;
          owner_mac[3] = 168;
          owner_mac[4] = 4;
          owner_mac[5] = disc_oct;
        }
        bool mac_assigned = false;
        for (size_t m = 0; m < count; ++m) {
          if (memcmp(fresh[m].mac, owner_mac, 6) == 0) {
            if (fresh[m].ip == 0) {
              fresh[m].ip = disc_ip;
              mac_assigned = true;
            } else {
              owner_mac[0] = 0x02;
              owner_mac[1] = 0xE0;
              owner_mac[2] = 192;
              owner_mac[3] = 168;
              owner_mac[4] = 4;
              owner_mac[5] = disc_oct;
            }
            break;
          }
        }
        if (!mac_assigned) {
          memcpy(fresh[count].mac, owner_mac, 6);
          fresh[count].ip = disc_ip;
          fresh[count].aid = 0xFF;
          fresh[count].rssi = -58;
          fresh[count].last_seen_s = now_s;
          count++;
        }
      }
    }
  }

  // Carry over active Range Extender / bridged clients (aid == 0xFF) that were seen
  // within BRIDGED_CLIENT_TIMEOUT_S even if lwIP's small ARP cache rotated their entry out
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (active_clients[k].aid == 0xFF && active_clients[k].ip != 0 &&
        valid_device_mac(active_clients[k].mac) &&
        (now_s <= active_clients[k].last_seen_s ||
         (now_s - active_clients[k].last_seen_s) <= BRIDGED_CLIENT_TIMEOUT_S)) {
      bool ip_found = false;
      for (size_t m = 0; m < count; ++m) {
        if (fresh[m].ip == active_clients[k].ip) {
          if (active_clients[k].last_seen_s > fresh[m].last_seen_s) {
            fresh[m].last_seen_s = active_clients[k].last_seen_s;
          }
          if (fresh[m].mac[0] == 0x02 && fresh[m].mac[1] == 0xE0 &&
              !(active_clients[k].mac[0] == 0x02 && active_clients[k].mac[1] == 0xE0)) {
            memcpy(fresh[m].mac, active_clients[k].mac, 6);
          }
          ip_found = true;
          break;
        }
      }
      if (!ip_found && count < MAX_CLIENTS) {
        active_client_t cand = active_clients[k];
        // Upgrade fallback 02:E0 MAC if a DHCP binding now exists for this IP
        if (cand.mac[0] == 0x02 && cand.mac[1] == 0xE0) {
          for (size_t b = 0; b < binding_count; ++b) {
            if (device_bindings[b].bound_ip == cand.ip &&
                valid_device_mac(device_bindings[b].mac)) {
              memcpy(cand.mac, device_bindings[b].mac, 6);
              break;
            }
          }
        }
        bool mac_exists = false;
        for (size_t m = 0; m < count; ++m) {
          if (memcmp(fresh[m].mac, cand.mac, 6) == 0) {
            if (fresh[m].ip == 0) {
              fresh[m].ip = cand.ip;
            }
            mac_exists = true;
            break;
          }
        }
        if (!mac_exists) {
          fresh[count++] = cand;
        }
      }
    }
  }

  memcpy(active_clients, fresh, sizeof(fresh));
  for (size_t i = 0; i < count; ++i) {
    meter_find_usage_locked(fresh[i].mac, true);
    meter_find_policy_locked(fresh[i].mac, true);
  }
  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);

  // Non-blocking STA reconnect handler: avoids stalling sys_evt or starving SoftAP association
  if (has_saved_config && !scan_running && !sta_connected_now && next_sta_reconnect_s > 0 && now_s >= next_sta_reconnect_s) {
    next_sta_reconnect_s = now_s + 15;
    esp_wifi_connect();
  }
}

bool wifi_manager_start_scan(void) {
  portENTER_CRITICAL(&data_mux);
  if (scan_running) {
    portEXIT_CRITICAL(&data_mux);
    ESP_LOGW(ROUTER_TAG, "Wi-Fi scan requested while already running");
    return false;
  }
  scan_running = true;
  scan_ready = false;
  scan_result_count = 0;
  portEXIT_CRITICAL(&data_mux);

  // Stop any pending scan or connection attempt
  esp_wifi_scan_stop();
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
  }

  esp_err_t err = esp_wifi_scan_start(NULL, false);
  if (err != ESP_OK) {
    ESP_LOGW(ROUTER_TAG, "esp_wifi_scan_start returned %s (0x%x), retrying...", esp_err_to_name(err), err);
    esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(150));
    err = esp_wifi_scan_start(NULL, false);
  }
  if (err != ESP_OK) {
    ESP_LOGE(ROUTER_TAG, "esp_wifi_scan_start failed: %s (0x%x)", esp_err_to_name(err), err);
    portENTER_CRITICAL(&data_mux);
    scan_running = false;
    scan_ready = true;
    scan_result_count = 0;
    portEXIT_CRITICAL(&data_mux);
    return false;
  }
  ESP_LOGI(ROUTER_TAG, "Wi-Fi asynchronous scan initiated across all channels");
  return true;
}

void wifi_manager_deauth_station(uint16_t aid) {
  esp_wifi_deauth_sta(aid);
}

const char *wifi_manager_get_current_band_str(void) {
  wifi_band_t cur_band = WIFI_BAND_2G;
  if (esp_wifi_get_band(&cur_band) == ESP_OK) {
    if (cur_band == WIFI_BAND_5G) return "5GHz";
    return "2.4GHz";
  }
  if (wifi_band_mode == 2) return "5GHz";
  if (wifi_band_mode == 1) return "2.4GHz";
  return "Dual-Band";
}

uint8_t wifi_manager_get_current_band(void) {
  wifi_band_t cur_band = WIFI_BAND_2G;
  if (esp_wifi_get_band(&cur_band) == ESP_OK) {
    return (cur_band == WIFI_BAND_5G) ? 2 : 1;
  }
  return wifi_band_mode;
}

void wifi_manager_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ap_netif = esp_netif_create_default_wifi_ap();
  sta_netif = esp_netif_create_default_wifi_sta();
  ESP_ERROR_CHECK(esp_netif_set_default_netif(sta_netif));

  if (has_saved_config && sta_static_enabled) {
    esp_netif_ip_info_t sta_ip = {0};
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_ip_text, &sta_ip.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_mask_text, &sta_ip.netmask));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_gateway_text, &sta_ip.gw));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &sta_ip));
    esp_netif_dns_info_t dns = {0};
    if (sta_dns_text[0]) ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_dns_text, &dns.ip.u_addr.ip4));
    else dns.ip.u_addr.ip4 = sta_ip.gw;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns));
  }

  esp_netif_ip_info_t ap_ip = {0};
  IP4_ADDR(&ap_ip.ip, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
  IP4_ADDR(&ap_ip.gw, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
  IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
  ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
  ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip));
  ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_config));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &on_wifi_event, NULL));

  // Determine Wi-Fi Band Mode for ESP32-C5:
  // 1 = 2.4GHz Only, 2 = 5GHz Only, 3 = Dual-Band Auto (2.4G + 5G)
  wifi_band_mode_t bmode = WIFI_BAND_MODE_AUTO;
  if (wifi_band_mode == 1) bmode = WIFI_BAND_MODE_2G_ONLY;
  else if (wifi_band_mode == 2) bmode = WIFI_BAND_MODE_5G_ONLY;

  // Set country policy to MANUAL with worldwide channels so 5GHz SoftAP + STA work seamlessly on all channels (36..165)
  wifi_country_t country = {
    .cc = "01",
    .schan = 1,
    .nchan = 13,
    .policy = WIFI_COUNTRY_POLICY_MANUAL,
  };
  esp_wifi_set_country(&country);

  wifi_config_t ap_config = {0};
  const char *selected_ap_ssid = has_saved_config ? ap_ssid : SETUP_SSID;
  const char *selected_ap_pass = has_saved_config ? ap_password : SETUP_PASSWORD;
  ap_config.ap.ssid_len = strnlen(selected_ap_ssid, sizeof(ap_config.ap.ssid));
  memcpy(ap_config.ap.ssid, selected_ap_ssid, ap_config.ap.ssid_len);
  memcpy(ap_config.ap.password, selected_ap_pass,
         strnlen(selected_ap_pass, sizeof(ap_config.ap.password) - 1));

  // Default SoftAP channel: 36 if 5G_ONLY, else 1 (automatically follows STA channel when STA connects to 5GHz/2.4GHz)
  ap_config.ap.channel = (bmode == WIFI_BAND_MODE_5G_ONLY) ? 36 : 1;

  ap_config.ap.max_connection = MAX_WIFI_STATIONS; // 12 max direct Wi-Fi connections
  if (strlen(selected_ap_pass) < 8) {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }
  ap_config.ap.pmf_cfg.capable = true;
  ap_config.ap.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  // Set band mode BEFORE esp_wifi_set_config(WIFI_IF_AP)
  esp_wifi_set_band_mode(bmode);
  if (esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK) {
    // Fallback if driver requires start() before band switch
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_band_mode(bmode);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  }

  if (has_saved_config) {
    wifi_config_t sta_config = {0};
    memcpy(sta_config.sta.ssid, router_ssid,
           strnlen(router_ssid, sizeof(sta_config.sta.ssid)));
    memcpy(sta_config.sta.password, router_password,
           strnlen(router_password, sizeof(sta_config.sta.password) - 1));
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.failure_retry_cnt = 2;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  }
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_country(&country);

  // Apply Wi-Fi Band Mode after esp_wifi_start()
  esp_err_t band_err = esp_wifi_set_band_mode(bmode);
  ESP_LOGI(ROUTER_TAG, "Wi-Fi band mode set to %d (%s) [result: %s]",
           (int)bmode,
           (bmode == WIFI_BAND_MODE_2G_ONLY) ? "2.4GHz Only" : (bmode == WIFI_BAND_MODE_5G_ONLY) ? "5GHz Only" : "Dual-Band Auto (2.4G + 5G)",
           esp_err_to_name(band_err));

  ESP_ERROR_CHECK(esp_wifi_set_ps(esp_wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));

  // Max TX Power: 18.0 dBm (72 * 0.25 dBm) - prevents USB current surge voltage dips
  esp_wifi_set_max_tx_power(72);

  // Bandwidth & Protocol Tuning with Wi-Fi 6 (802.11ax) acceleration
  if (bmode == WIFI_BAND_MODE_AUTO) {
    // Enable BOTH 2.4GHz and 5GHz protocols on SoftAP and STA!
    // Because ESP32-C5 shares a single radio, when STA connects to a 5GHz router, SoftAP also operates on 5GHz!
    wifi_protocols_t dual_protos = {
      .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
      .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    esp_wifi_set_protocols(WIFI_IF_AP, &dual_protos);
    esp_wifi_set_protocols(WIFI_IF_STA, &dual_protos);

    wifi_bandwidths_t ap_bws = {
      .ghz_2g = WIFI_BW20,
      .ghz_5g = WIFI_BW20,
    };
    wifi_bandwidths_t sta_bws = {
      .ghz_2g = (wifi_protocol_mode == 0 || wifi_protocol_mode == 3) ? WIFI_BW20 : WIFI_BW40,
      .ghz_5g = (wifi_protocol_mode == 0 || wifi_protocol_mode == 3) ? WIFI_BW20 : WIFI_BW40,
    };
    esp_wifi_set_bandwidths(WIFI_IF_AP, &ap_bws);
    esp_wifi_set_bandwidths(WIFI_IF_STA, &sta_bws);
    ESP_LOGI(ROUTER_TAG, "Wi-Fi radio tuned: ESP32-C5 Dual-Band (AP+STA 2.4G/5G 11a/b/g/n/ac/ax, max_conn=%d)",
             MAX_WIFI_STATIONS);
  } else if (bmode == WIFI_BAND_MODE_5G_ONLY) {
    uint16_t proto_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
    wifi_bandwidth_t bw = (wifi_protocol_mode == 0 || wifi_protocol_mode == 3) ? WIFI_BW20 : WIFI_BW40;
    esp_wifi_set_protocol(WIFI_IF_AP, proto_5g);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    esp_wifi_set_protocol(WIFI_IF_STA, proto_5g);
    esp_wifi_set_bandwidth(WIFI_IF_STA, bw);
    ESP_LOGI(ROUTER_TAG, "Wi-Fi radio tuned: ESP32-C5 5GHz Only (11a/n/ac/ax Wi-Fi 6, Ch 36, max_conn=%d)",
             MAX_WIFI_STATIONS);
  } else {
    uint8_t proto_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    wifi_bandwidth_t bw = (wifi_protocol_mode == 0 || wifi_protocol_mode == 3) ? WIFI_BW20 : WIFI_BW40;
    esp_wifi_set_protocol(WIFI_IF_AP, proto_2g);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    esp_wifi_set_protocol(WIFI_IF_STA, proto_2g);
    esp_wifi_set_bandwidth(WIFI_IF_STA, bw);
    ESP_LOGI(ROUTER_TAG, "Wi-Fi radio tuned: ESP32-C5 2.4GHz Only (11bgnax Wi-Fi 6, bw=%s, Ch 1, max_conn=%d)",
             (bw == WIFI_BW40) ? "HT40" : "HT20", MAX_WIFI_STATIONS);
  }

  ESP_LOGI(ROUTER_TAG, "=================================================");
  ESP_LOGI(ROUTER_TAG, ">>> SoftAP Broadcast Active <<<");
  ESP_LOGI(ROUTER_TAG, "SSID:      '%s'", selected_ap_ssid);
  ESP_LOGI(ROUTER_TAG, "Password:  '%s'", selected_ap_pass);
  ESP_LOGI(ROUTER_TAG, "Channel:   %d", ap_config.ap.channel);
  ESP_LOGI(ROUTER_TAG, "Band Mode: %s", (bmode == WIFI_BAND_MODE_5G_ONLY) ? "5GHz Only" : (bmode == WIFI_BAND_MODE_2G_ONLY) ? "2.4GHz Only" : "Dual-Band Auto (SoftAP: 2.4G Wi-Fi 6)");
  ESP_LOGI(ROUTER_TAG, "Auth Mode: %s", (ap_config.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2-PSK");
  ESP_LOGI(ROUTER_TAG, "=================================================");
}

