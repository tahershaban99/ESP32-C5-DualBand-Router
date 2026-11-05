#include "wifi_manager.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/lwip_napt.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "dhcpserver/dhcpserver.h"
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

typedef struct dhcps_list_node {
  void *pnode;
  struct dhcps_list_node *pnext;
} dhcps_list_node_t;

typedef enum {
  DHCPS_STATE_CREATED_INT = 0,
  DHCPS_STATE_STARTED_INT,
  DHCPS_STATE_STOPPED_INT,
  DHCPS_STATE_DELETE_PENDING_INT,
} dhcps_handle_state_int_t;

struct dhcps_internal_s {
  struct netif *dhcps_netif;
  ip4_addr_t broadcast_dhcps;
  ip4_addr_t server_address;
  ip4_addr_t dns_server[DNS_TYPE_MAX];
  ip4_addr_t client_address;
  ip4_addr_t client_address_plus;
  ip4_addr_t dhcps_mask;
  dhcps_list_node_t *plist;
  bool renew;
  dhcps_lease_t dhcps_poll;
  dhcps_time_t dhcps_lease_time;
  dhcps_offer_t dhcps_offer;
  dhcps_offer_t dhcps_dns;
  char *dhcps_captiveportal_uri;
  dhcps_cb_t dhcps_cb;
  void *dhcps_cb_arg;
  struct udp_pcb *dhcps_pcb;
  dhcps_handle_state_int_t state;
  bool has_declined_ip;
#if CONFIG_LWIP_DHCPS_REPORT_CLIENT_HOSTNAME
  char opt_hostname[CONFIG_LWIP_DHCPS_MAX_HOSTNAME_LEN];
  bool opt_hostname_present;
#endif
};

static udp_recv_fn orig_dhcps_recv = NULL;

static void dhcps_list_insert_sorted(dhcps_list_node_t **phead, dhcps_list_node_t *pinsert) {
  pinsert->pnext = NULL;
  if (*phead == NULL) {
    *phead = pinsert;
    return;
  }
  struct dhcps_pool *ins_pool = (struct dhcps_pool *)pinsert->pnode;
  struct dhcps_pool *head_pool = (struct dhcps_pool *)(*phead)->pnode;
  if (ins_pool->ip.addr < head_pool->ip.addr) {
    pinsert->pnext = *phead;
    *phead = pinsert;
    return;
  }
  dhcps_list_node_t *cur = *phead;
  while (cur->pnext != NULL) {
    struct dhcps_pool *next_pool = (struct dhcps_pool *)cur->pnext->pnode;
    if (ins_pool->ip.addr < next_pool->ip.addr) {
      pinsert->pnext = cur->pnext;
      cur->pnext = pinsert;
      return;
    }
    cur = cur->pnext;
  }
  cur->pnext = pinsert;
}

static void dhcps_list_remove(dhcps_list_node_t **phead, dhcps_list_node_t *pdelete) {
  if (*phead == NULL || pdelete == NULL) return;
  if (*phead == pdelete) {
    *phead = pdelete->pnext;
    pdelete->pnext = NULL;
    return;
  }
  dhcps_list_node_t *cur = *phead;
  while (cur->pnext != NULL) {
    if (cur->pnext == pdelete) {
      cur->pnext = pdelete->pnext;
      pdelete->pnext = NULL;
      return;
    }
    cur = cur->pnext;
  }
}

static void dhcps_sync_internal(struct dhcps_internal_s *dhcps) {
  if (!dhcps || dhcps->state != DHCPS_STATE_STARTED_INT) return;

  device_binding_t local_bindings[MAX_RECORDS];
  size_t b_cnt = 0;
  portENTER_CRITICAL(&data_mux);
  b_cnt = binding_count <= MAX_RECORDS ? binding_count : MAX_RECORDS;
  if (b_cnt > 0) {
    memcpy(local_bindings, device_bindings, b_cnt * sizeof(device_binding_t));
  }
  portEXIT_CRITICAL(&data_mux);

  // 1. Ensure all persisted device_bindings exist in dhcps->plist so DHCP never
  // hands out an IP that belongs to another Range Extender or direct device!
  for (size_t b = 0; b < b_cnt; ++b) {
    uint32_t bip = local_bindings[b].bound_ip;
    const uint8_t *bmac = local_bindings[b].mac;
    uint8_t oct = ((const uint8_t *)&bip)[3];
    if (!valid_device_mac(bmac) || (bmac[0] == 0x02 && (bmac[1] == 0x00 || bmac[1] == 0xE0))) continue;
    if ((bip & 0x00FFFFFF) != 0x0004A8C0 || oct < 2 || oct > 254) continue;

    dhcps_list_node_t *found_mac_node = NULL;
    dhcps_list_node_t *found_ip_node = NULL;
    for (dhcps_list_node_t *p = dhcps->plist; p != NULL; p = p->pnext) {
      struct dhcps_pool *pool = (struct dhcps_pool *)p->pnode;
      if (!pool) continue;
      if (memcmp(pool->mac, bmac, 6) == 0 || memcmp(pool->mac + 3, bmac + 3, 3) == 0) {
        found_mac_node = p;
      }
      if (pool->ip.addr == bip) {
        found_ip_node = p;
      }
    }

    if (found_mac_node != NULL) {
      struct dhcps_pool *pool = (struct dhcps_pool *)found_mac_node->pnode;
      pool->lease_timer = 0x7FFFFFFF;
      if (pool->ip.addr != bip) {
        if (found_ip_node != NULL && found_ip_node != found_mac_node) {
          dhcps_list_remove(&dhcps->plist, found_ip_node);
          if (found_ip_node->pnode) mem_free(found_ip_node->pnode);
          mem_free(found_ip_node);
        }
        dhcps_list_remove(&dhcps->plist, found_mac_node);
        pool->ip.addr = bip;
        dhcps_list_insert_sorted(&dhcps->plist, found_mac_node);
      }
    } else if (found_ip_node == NULL) {
      struct dhcps_pool *new_pool = (struct dhcps_pool *)mem_calloc(1, sizeof(struct dhcps_pool));
      if (new_pool) {
        dhcps_list_node_t *new_node = (dhcps_list_node_t *)mem_calloc(1, sizeof(dhcps_list_node_t));
        if (new_node) {
          new_pool->ip.addr = bip;
          memcpy(new_pool->mac, bmac, 6);
          new_pool->lease_timer = 0x7FFFFFFF;
          new_node->pnode = new_pool;
          dhcps_list_insert_sorted(&dhcps->plist, new_node);
        } else {
          mem_free(new_pool);
        }
      }
    } else {
      struct dhcps_pool *ip_pool = (struct dhcps_pool *)found_ip_node->pnode;
      if (ip_pool) {
        memcpy(ip_pool->mac, bmac, 6);
        ip_pool->lease_timer = 0x7FFFFFFF;
      }
    }
  }

  // 2. Pull any newly assigned DHCP leases from dhcps->plist into device_bindings
  // without overwriting existing static/bound IPs in device_bindings
  bool bindings_updated = false;
  for (dhcps_list_node_t *p = dhcps->plist; p != NULL; p = p->pnext) {
    struct dhcps_pool *pool = (struct dhcps_pool *)p->pnode;
    if (!pool || !valid_device_mac(pool->mac) || pool->ip.addr == 0) continue;
    pool->lease_timer = 0x7FFFFFFF;

    portENTER_CRITICAL(&data_mux);
    uint8_t canon_mac[6];
    meter_resolve_canonical_mac_locked(pool->mac, false, canon_mac);
    int idx = meter_find_binding_locked(canon_mac, true);
    if (idx >= 0 && device_bindings[idx].bound_ip == 0) {
      bool ip_claimed = false;
      for (size_t k = 0; k < binding_count; ++k) {
        if ((int)k != idx && device_bindings[k].bound_ip == pool->ip.addr) {
          ip_claimed = true;
          break;
        }
      }
      if (!ip_claimed) {
        device_bindings[idx].bound_ip = pool->ip.addr;
        bindings_updated = true;
      }
    }
    portEXIT_CRITICAL(&data_mux);
  }

  if (bindings_updated) {
    meter_save_usage();
  }
}

static void router_dhcps_recv_wrapper(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                      const ip_addr_t *addr, u16_t port) {
  struct dhcps_internal_s *dhcps = (struct dhcps_internal_s *)arg;
  uint8_t pkt_chaddr[6] = {0};
  bool has_pkt_chaddr = false;
  if (dhcps && dhcps->dhcps_pcb == pcb && p != NULL && p->tot_len >= 240) {
    dhcps_sync_internal(dhcps);

    uint8_t ciaddr[4] = {0};
    uint8_t chaddr[6] = {0};
    uint8_t cookie[4] = {0};
    pbuf_copy_partial(p, ciaddr, 4, 12);
    pbuf_copy_partial(p, chaddr, 6, 28);
    pbuf_copy_partial(p, cookie, 4, 236);

    if (valid_device_mac(chaddr) &&
        cookie[0] == 0x63 && cookie[1] == 0x82 && cookie[2] == 0x53 && cookie[3] == 0x63) {
      memcpy(pkt_chaddr, chaddr, 6);
      has_pkt_chaddr = true;
      // Parse Option 50 (Requested IP Address) if present
      uint32_t req_ip = 0;
      memcpy(&req_ip, ciaddr, 4);
      uint8_t opts[128];
      u16_t opt_avail = (p->tot_len > 240) ? (u16_t)(p->tot_len - 240) : 0;
      if (opt_avail > sizeof(opts)) opt_avail = sizeof(opts);
      if (opt_avail > 0 && pbuf_copy_partial(p, opts, opt_avail, 240) == opt_avail) {
        u16_t pos = 0;
        while (pos < opt_avail) {
          uint8_t code = opts[pos];
          if (code == 0) { pos++; continue; }
          if (code == 255 || pos + 1 >= opt_avail) break;
          uint8_t olen = opts[pos + 1];
          if (pos + 2 + olen > opt_avail) break;
          if (code == 50 && olen == 4) {
            memcpy(&req_ip, &opts[pos + 2], 4);
            break;
          }
          pos += (u16_t)(olen + 2);
        }
      }

      uint8_t canon_mac[6];
      portENTER_CRITICAL(&data_mux);
      meter_resolve_canonical_mac_locked(chaddr, false, canon_mac);
      portEXIT_CRITICAL(&data_mux);

      dhcps_list_node_t *matched_node = NULL;
      for (dhcps_list_node_t *n = dhcps->plist; n != NULL; n = n->pnext) {
        struct dhcps_pool *pool = (struct dhcps_pool *)n->pnode;
        if (!pool) continue;
        if (memcmp(pool->mac, chaddr, 6) == 0 ||
            memcmp(pool->mac, canon_mac, 6) == 0 ||
            memcmp(pool->mac + 3, chaddr + 3, 3) == 0) {
          matched_node = n;
          break;
        }
      }

      if (matched_node != NULL) {
        // Ensure pool->mac matches packet chaddr so handle_dhcp finds this exact lease
        struct dhcps_pool *pool = (struct dhcps_pool *)matched_node->pnode;
        memcpy(pool->mac, chaddr, 6);
        pool->lease_timer = 0x7FFFFFFF;
      } else if (req_ip != 0 && (req_ip & 0x00FFFFFF) == 0x0004A8C0) {
        uint8_t req_oct = ((const uint8_t *)&req_ip)[3];
        if (req_oct >= 2 && req_oct <= 254) {
          bool ip_taken = false;
          for (dhcps_list_node_t *n = dhcps->plist; n != NULL; n = n->pnext) {
            struct dhcps_pool *pool = (struct dhcps_pool *)n->pnode;
            if (pool && pool->ip.addr == req_ip) {
              ip_taken = true;
              break;
            }
          }
          if (!ip_taken) {
            // Pre-insert requested IP into dhcps->plist so handle_dhcp ACKs existing client IP
            struct dhcps_pool *new_pool = (struct dhcps_pool *)mem_calloc(1, sizeof(struct dhcps_pool));
            if (new_pool) {
              dhcps_list_node_t *new_node = (dhcps_list_node_t *)mem_calloc(1, sizeof(dhcps_list_node_t));
              if (new_node) {
                new_pool->ip.addr = req_ip;
                memcpy(new_pool->mac, chaddr, 6);
                new_pool->lease_timer = 0x7FFFFFFF;
                new_node->pnode = new_pool;
                dhcps_list_insert_sorted(&dhcps->plist, new_node);
              } else {
                mem_free(new_pool);
              }
            }
          }
        }
      }
    }
  }

  if (orig_dhcps_recv) {
    orig_dhcps_recv(arg, pcb, p, addr, port);
  } else if (p) {
    pbuf_free(p);
  }

  if (has_pkt_chaddr && dhcps && dhcps->state == DHCPS_STATE_STARTED_INT) {
    for (dhcps_list_node_t *n = dhcps->plist; n != NULL; n = n->pnext) {
      struct dhcps_pool *pool = (struct dhcps_pool *)n->pnode;
      if (pool && pool->ip.addr != 0 &&
          (memcmp(pool->mac, pkt_chaddr, 6) == 0 || memcmp(pool->mac + 3, pkt_chaddr + 3, 3) == 0)) {
        meter_sync_device_on_dhcp(pkt_chaddr, pool->ip.addr);
        break;
      }
    }
  }
}

static netif_input_fn orig_ap_netif_input = NULL;

static err_t router_ap_netif_input(struct pbuf *p, struct netif *inp) {
  if (p != NULL && p->len >= 28) {
    uint8_t eth_hdr[14];
    if (pbuf_copy_partial(p, eth_hdr, 14, 0) == 14) {
      uint16_t eth_type = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];
      if (eth_type == 0x0806) { // ARP packet
        uint8_t arp_sender_ip[4];
        if (pbuf_copy_partial(p, arp_sender_ip, 4, 28) == 4) {
          if (arp_sender_ip[0] == AP_IP_A && arp_sender_ip[1] == AP_IP_B && arp_sender_ip[2] == AP_IP_C) {
            uint8_t oct = arp_sender_ip[3];
            if (oct >= 2 && oct <= 254) {
              uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
              meter_notify_client_active_ip(oct, now_s);
            }
          }
        }
      }
    }
  }
  return orig_ap_netif_input ? orig_ap_netif_input(p, inp) : pbuf_free(p);
}

static void arp_probe_tcpip_cb(void *ctx) {
  uint32_t target_ip = (uint32_t)(uintptr_t)ctx;
  if (ap_netif == NULL) return;
  struct netif *lwip_ap = (struct netif *)esp_netif_get_netif_impl(ap_netif);
  if (lwip_ap != NULL) {
    ip4_addr_t tip;
    tip.addr = target_ip;
    etharp_request(lwip_ap, &tip);
  }
}

void wifi_manager_send_arp_probe(uint32_t ip) {
  if (ip == 0 || ap_netif == NULL) return;
  tcpip_try_callback(arp_probe_tcpip_cb, (void *)(uintptr_t)ip);
}

static void arp_invalidate_tcpip_cb(void *ctx) {
  uint32_t ip_to_clear = (uint32_t)(uintptr_t)ctx;
  for (size_t i = 0; i < ARP_TABLE_SIZE; ++i) {
    ip4_addr_t *ip = NULL;
    struct netif *nf = NULL;
    struct eth_addr *eth = NULL;
    if (etharp_get_entry(i, &ip, &nf, &eth) && ip && ip->addr == ip_to_clear) {
      ip->addr = 0;
      if (eth) memset(eth->addr, 0, 6);
    }
  }
}

void wifi_manager_invalidate_arp_entry(uint32_t ip) {
  if (ip == 0) return;
  tcpip_try_callback(arp_invalidate_tcpip_cb, (void *)(uintptr_t)ip);
}

typedef struct {
  uint8_t mac[6];
  uint32_t disc_s;
} direct_disc_entry_t;

static direct_disc_entry_t recent_direct_disc[4] = {0};
static size_t recent_direct_disc_idx = 0;

static void record_direct_disconnect(const uint8_t mac[6]) {
  uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  memcpy(recent_direct_disc[recent_direct_disc_idx].mac, mac, 6);
  recent_direct_disc[recent_direct_disc_idx].disc_s = now_s;
  recent_direct_disc_idx = (recent_direct_disc_idx + 1) % 4;
}

static bool is_recent_direct_disconnect(const uint8_t mac[6], uint32_t now_s) {
  for (size_t i = 0; i < 4; ++i) {
    if (memcmp(recent_direct_disc[i].mac, mac, 6) == 0) {
      if (now_s >= recent_direct_disc[i].disc_s && (now_s - recent_direct_disc[i].disc_s) < 30) {
        return true;
      }
    }
  }
  return false;
}

static void dhcps_sync_tcpip_cb(void *ctx) {
  (void)ctx;
  if (ap_netif != NULL) {
    struct netif *lwip_ap = (struct netif *)esp_netif_get_netif_impl(ap_netif);
    if (lwip_ap != NULL && lwip_ap->input != router_ap_netif_input) {
      orig_ap_netif_input = lwip_ap->input;
      lwip_ap->input = router_ap_netif_input;
    }
  }
  uint32_t expected_srv_ip = PP_HTONL(LWIP_MAKEU32(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D));
  for (struct udp_pcb *pcb = udp_pcbs; pcb != NULL; pcb = pcb->next) {
    if (pcb->local_port == 67 && pcb->recv_arg != NULL) {
      struct dhcps_internal_s *dhcps = (struct dhcps_internal_s *)pcb->recv_arg;
      if (dhcps->dhcps_pcb == pcb && dhcps->server_address.addr == expected_srv_ip) {
        if (pcb->recv != NULL && pcb->recv != router_dhcps_recv_wrapper) {
          orig_dhcps_recv = pcb->recv;
          pcb->recv = router_dhcps_recv_wrapper;
        }
        dhcps_sync_internal(dhcps);
        break;
      }
    }
  }
}

void wifi_manager_advertise_uplink_dns(void) {
  esp_netif_dns_info_t dns = {0};
  if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) return;
  // Update DNS server on ap_netif WITHOUT calling esp_netif_dhcps_stop(),
  // preserving all active DHCP leases in dhcps->plist!
  esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
  tcpip_try_callback(dhcps_sync_tcpip_cb, NULL);
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
  tcpip_try_callback(dhcps_sync_tcpip_cb, NULL);
  portENTER_CRITICAL(&data_mux);
  uint8_t canon_mac[6];
  meter_resolve_canonical_mac_locked(event->mac, true, canon_mac);
  int b_idx = meter_find_binding_locked(canon_mac, false);
  uint32_t known_ip = (b_idx >= 0) ? device_bindings[b_idx].bound_ip : 0;

  if (known_ip != 0) {
    size_t slot = MAX_CLIENTS;
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
      if (memcmp(active_clients[i].mac, canon_mac, 6) == 0) {
        slot = i;
        break;
      }
      if (slot == MAX_CLIENTS && !valid_device_mac(active_clients[i].mac)) slot = i;
    }
    if (slot < MAX_CLIENTS) {
      memcpy(active_clients[slot].mac, canon_mac, 6);
      active_clients[slot].ip = known_ip;
      active_clients[slot].aid = event->aid;
      active_clients[slot].last_seen_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    }
    meter_find_usage_locked(canon_mac, true);
    meter_find_policy_locked(canon_mac, true);
    meter_rebuild_fast_lookup();
  }
  portEXIT_CRITICAL(&data_mux);
}

static void handle_ap_station_disconnected(const wifi_event_ap_stadisconnected_t *event) {
  if (!event) return;
  uint8_t canon_mac[6];
  uint32_t disc_ip = 0;

  portENTER_CRITICAL(&data_mux);
  meter_resolve_canonical_mac_locked(event->mac, false, canon_mac);
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, event->mac, 6) == 0 ||
        memcmp(active_clients[i].mac, canon_mac, 6) == 0) {
      disc_ip = active_clients[i].ip;
      active_clients[i].aid = 0;
      active_clients[i].ip = 0;
      memset(active_clients[i].mac, 0, 6);
      break;
    }
  }
  if (disc_ip != 0) {
    uint8_t oct = ((uint8_t *)&disc_ip)[3];
    if (oct >= 2 && oct <= 254) {
      ip_lookup[oct] = NULL;
      for (size_t k = 0; k < MAX_CLIENTS; ++k) {
        if (fast_device_pool[k].ip == disc_ip) {
          memset(&fast_device_pool[k], 0, sizeof(fast_device_pool[k]));
        }
      }
    }
  }
  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);

  if (disc_ip != 0) {
    wifi_manager_invalidate_arp_entry(disc_ip);
  }
  record_direct_disconnect(canon_mac);
  record_direct_disconnect(event->mac);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
    tcpip_try_callback(dhcps_sync_tcpip_cb, NULL);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
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
  tcpip_try_callback(dhcps_sync_tcpip_cb, NULL);

  wifi_sta_list_t wifi_list = {0};
  esp_netif_pair_mac_ip_t ip_list[MAX_CLIENTS] = {0};
  active_client_t fresh[MAX_CLIENTS] = {0};
  if (esp_wifi_ap_get_sta_list(&wifi_list) != ESP_OK) return;

  uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

  size_t sta_count = wifi_list.num < MAX_WIFI_STATIONS ? wifi_list.num : MAX_WIFI_STATIONS;
  for (size_t i = 0; i < sta_count; ++i) {
    memcpy(ip_list[i].mac, wifi_list.sta[i].mac, 6);
  }
  if (sta_count > 0) {
    esp_netif_dhcps_get_clients_by_mac(ap_netif, sta_count, ip_list);
  }

  size_t count = sta_count;
  portENTER_CRITICAL(&data_mux);
  for (size_t i = 0; i < sta_count; ++i) {
    meter_resolve_canonical_mac_locked(wifi_list.sta[i].mac, true, fresh[i].mac);
    fresh[i].rssi = wifi_list.sta[i].rssi;
    fresh[i].last_seen_s = now_s;
    fresh[i].ip = ip_list[i].ip.addr;
    if (fresh[i].ip != 0) {
      int b_idx = meter_find_binding_locked(fresh[i].mac, true);
      if (b_idx >= 0 && device_bindings[b_idx].bound_ip != fresh[i].ip) {
        device_bindings[b_idx].bound_ip = fresh[i].ip;
      }
    }
    uint8_t prev_aid = 0;
    for (size_t k = 0; k < MAX_CLIENTS; ++k) {
      if (memcmp(active_clients[k].mac, fresh[i].mac, 6) == 0 &&
          active_clients[k].aid != 0 && active_clients[k].aid != 0xFF) {
        prev_aid = active_clients[k].aid;
        break;
      }
    }
    fresh[i].aid = (prev_aid != 0) ? prev_aid : (uint8_t)(i + 1);
  }
  portEXIT_CRITICAL(&data_mux);

  // Query lwIP ARP table for devices using a static IP or connected via Range Extender (Proxy-ARP / Bridge)
  for (size_t a = 0; a < ARP_TABLE_SIZE; ++a) {
    ip4_addr_t *arp_ip = NULL;
    struct netif *arp_netif = NULL;
    struct eth_addr *arp_eth = NULL;
    if (etharp_get_entry(a, &arp_ip, &arp_netif, &arp_eth) && arp_ip && arp_eth && arp_ip->addr != 0) {
      uint32_t ip4 = arp_ip->addr;
      if ((ip4 & 0x00FFFFFF) == 0x0004A8C0 && (ip4 >> 24) != 1 && valid_device_mac(arp_eth->addr)) {
        // Count how many ARP entries share arp_eth->addr to detect Proxy-ARP Range Extenders
        size_t eth_arp_count = 0;
        for (size_t a2 = 0; a2 < ARP_TABLE_SIZE; ++a2) {
          ip4_addr_t *ip2 = NULL;
          struct netif *nf2 = NULL;
          struct eth_addr *eth2 = NULL;
          if (etharp_get_entry(a2, &ip2, &nf2, &eth2) && ip2 && eth2 &&
              (ip2->addr & 0x00FFFFFF) == 0x0004A8C0 && (ip2->addr >> 24) != 1 &&
              memcmp(eth2->addr, arp_eth->addr, 6) == 0) {
            eth_arp_count++;
          }
        }

        // Check if DHCP binding table knows the true client MAC for this IP
        uint8_t bound_mac[6] = {0};
        bool has_bound_mac = false;
        portENTER_CRITICAL(&data_mux);
        for (size_t b = 0; b < binding_count; ++b) {
          if (device_bindings[b].bound_ip == ip4 && valid_device_mac(device_bindings[b].mac) &&
              !(device_bindings[b].mac[0] == 0x02 &&
                (device_bindings[b].mac[1] == 0x00 || device_bindings[b].mac[1] == 0xE0))) {
            meter_resolve_canonical_mac_locked(device_bindings[b].mac, false, bound_mac);
            has_bound_mac = true;
            break;
          }
        }
        portEXIT_CRITICAL(&data_mux);

        bool is_proxy_mac = (eth_arp_count > 1) ||
                            (has_bound_mac &&
                             memcmp(bound_mac, arp_eth->addr, 6) != 0 &&
                             memcmp(bound_mac + 3, arp_eth->addr + 3, 3) != 0);

        // If a single non-proxy client switched from its old DHCP IP to a new Static IP,
        // both the old IP (stale ARP) and new IP may briefly appear in ARP with the same MAC.
        // Detect if eth_arp_count == 2 is just one client switching from its old bound_ip:
        if (eth_arp_count == 2 && !has_bound_mac) {
          bool other_is_own_old_ip = false;
          portENTER_CRITICAL(&data_mux);
          uint8_t resolved_self[6];
          meter_resolve_canonical_mac_locked(arp_eth->addr, false, resolved_self);
          int self_b = meter_find_binding_locked(resolved_self, false);
          uint32_t self_old_ip = (self_b >= 0) ? device_bindings[self_b].bound_ip : 0;
          bool any_other_client_bound = false;
          for (size_t b = 0; b < binding_count; ++b) {
            if ((int)b != self_b && device_bindings[b].bound_ip != 0 &&
                memcmp(device_bindings[b].mac, resolved_self, 6) != 0 &&
                memcmp(device_bindings[b].mac + 3, arp_eth->addr + 3, 3) != 0) {
              // Check if any of the ARP IPs on arp_eth->addr belong to another client's bound_ip
              for (size_t a2 = 0; a2 < ARP_TABLE_SIZE; ++a2) {
                ip4_addr_t *ip2 = NULL;
                struct netif *nf2 = NULL;
                struct eth_addr *eth2 = NULL;
                if (etharp_get_entry(a2, &ip2, &nf2, &eth2) && ip2 && eth2 &&
                    memcmp(eth2->addr, arp_eth->addr, 6) == 0 &&
                    ip2->addr == device_bindings[b].bound_ip) {
                  any_other_client_bound = true;
                  break;
                }
              }
            }
          }
          portEXIT_CRITICAL(&data_mux);
          if (self_old_ip != 0 && self_old_ip != ip4 && !any_other_client_bound) {
            for (size_t a2 = 0; a2 < ARP_TABLE_SIZE; ++a2) {
              ip4_addr_t *ip2 = NULL;
              struct netif *nf2 = NULL;
              struct eth_addr *eth2 = NULL;
              if (etharp_get_entry(a2, &ip2, &nf2, &eth2) && ip2 && eth2 &&
                  memcmp(eth2->addr, arp_eth->addr, 6) == 0 && ip2->addr == self_old_ip) {
                other_is_own_old_ip = true;
                break;
              }
            }
          }
          if (other_is_own_old_ip) {
            is_proxy_mac = false;
          }
        }

        // 1. Skip if this IP is already assigned in fresh[]
        bool matched = false;
        for (size_t k = 0; k < count; ++k) {
          if (fresh[k].ip == ip4) {
            matched = true;
            break;
          }
        }
        if (matched) continue;

        // Skip if this MAC recently disconnected directly from SoftAP
        if (is_recent_direct_disconnect(arp_eth->addr, now_s)) continue;
        if (has_bound_mac && is_recent_direct_disconnect(bound_mac, now_s)) continue;

        // Determine real last seen time for this client
        uint32_t assigned_last_seen = 0;
        for (size_t k = 0; k < MAX_CLIENTS; ++k) {
          if (active_clients[k].ip == ip4 || fast_device_pool[k].ip == ip4) {
            if (active_clients[k].last_seen_s > assigned_last_seen) assigned_last_seen = active_clients[k].last_seen_s;
            if (fast_device_pool[k].last_seen_s > assigned_last_seen) assigned_last_seen = fast_device_pool[k].last_seen_s;
          }
        }
        if (assigned_last_seen == 0) assigned_last_seen = now_s;

        // 2. Assign to bound_mac from DHCP/Static bindings
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
            fresh[count].last_seen_s = assigned_last_seen;
            count++;
            matched = true;
          }
        }

        // 3. Non-proxy unique ARP MAC (MAT Range Extender client or direct static-IP device)
        if (!matched && !is_proxy_mac) {
          uint8_t resolved_mac[6];
          portENTER_CRITICAL(&data_mux);
          meter_resolve_canonical_mac_locked(arp_eth->addr, false, resolved_mac);
          int b_idx = meter_find_binding_locked(resolved_mac, true);
          if (b_idx >= 0 && device_bindings[b_idx].bound_ip != ip4) {
            device_bindings[b_idx].bound_ip = ip4;
            for (size_t k = 0; k < binding_count; ++k) {
              if ((int)k != b_idx && device_bindings[k].bound_ip == ip4) {
                device_bindings[k].bound_ip = 0;
              }
            }
          }
          uint8_t vmac[6] = {0x02, 0xE0, 192, 168, 4, ((const uint8_t *)&ip4)[3]};
          meter_merge_mac_locked(vmac, resolved_mac);
          portEXIT_CRITICAL(&data_mux);

          for (size_t k = 0; k < count; ++k) {
            if (memcmp(fresh[k].mac, resolved_mac, 6) == 0) {
              fresh[k].ip = ip4;
              matched = true;
              break;
            }
          }
          if (!matched && count < MAX_CLIENTS) {
            memcpy(fresh[count].mac, resolved_mac, 6);
            fresh[count].ip = ip4;
            fresh[count].aid = 0xFF;
            fresh[count].rssi = -55;
            fresh[count].last_seen_s = assigned_last_seen;
            count++;
            matched = true;
          }
        }

        // 4. Fallback for unbound Static-IP client behind a Proxy-ARP Range Extender:
        // synthesize a deterministic per-IP virtual MAC (02:E0:192:168:4:XX) so the device
        // appears as Connected with its Static IP and can be metered/controlled.
        if (!matched && count < MAX_CLIENTS) {
          uint8_t vmac[6] = {0x02, 0xE0, 192, 168, 4, ((const uint8_t *)&ip4)[3]};
          for (size_t k = 0; k < count; ++k) {
            if (memcmp(fresh[k].mac, vmac, 6) == 0) {
              fresh[k].ip = ip4;
              matched = true;
              break;
            }
          }
          if (!matched) {
            memcpy(fresh[count].mac, vmac, 6);
            fresh[count].ip = ip4;
            fresh[count].aid = 0xFF;
            fresh[count].rssi = -55;
            fresh[count].last_seen_s = assigned_last_seen;
            count++;
          }
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
          if (device_bindings[b].bound_ip == disc_ip && valid_device_mac(device_bindings[b].mac) &&
              !(device_bindings[b].mac[0] == 0x02 &&
                (device_bindings[b].mac[1] == 0x00 || device_bindings[b].mac[1] == 0xE0))) {
            meter_resolve_canonical_mac_locked(device_bindings[b].mac, false, owner_mac);
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
        if (valid_device_mac(owner_mac)) {
          bool mac_assigned = false;
          for (size_t m = 0; m < count; ++m) {
            if (memcmp(fresh[m].mac, owner_mac, 6) == 0) {
              if (fresh[m].ip == 0) {
                fresh[m].ip = disc_ip;
                mac_assigned = true;
              } else if (fresh[m].ip == disc_ip) {
                mac_assigned = true;
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
  }

  // Carry over active Range Extender / bridged clients (aid == 0xFF) that were seen
  // within BRIDGED_CLIENT_TIMEOUT_S even if lwIP's small ARP cache rotated their entry out
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (active_clients[k].aid == 0xFF && active_clients[k].ip != 0 &&
         valid_device_mac(active_clients[k].mac) &&
        !(active_clients[k].mac[0] == 0x02 && active_clients[k].mac[1] == 0x00) &&
        !is_recent_direct_disconnect(active_clients[k].mac, now_s) &&
        (now_s <= active_clients[k].last_seen_s ||
         (now_s - active_clients[k].last_seen_s) <= BRIDGED_CLIENT_TIMEOUT_S)) {
      bool ip_found = false;
      for (size_t m = 0; m < count; ++m) {
        if (fresh[m].ip == active_clients[k].ip) {
          if (active_clients[k].last_seen_s > fresh[m].last_seen_s) {
            fresh[m].last_seen_s = active_clients[k].last_seen_s;
          }
          ip_found = true;
          break;
        }
      }
      if (!ip_found && count < MAX_CLIENTS) {
        active_client_t cand = active_clients[k];
        if (cand.mac[0] == 0x02 && cand.mac[1] == 0xE0) {
          for (size_t b = 0; b < binding_count; ++b) {
            if (device_bindings[b].bound_ip == cand.ip && valid_device_mac(device_bindings[b].mac) &&
                !(device_bindings[b].mac[0] == 0x02 &&
                  (device_bindings[b].mac[1] == 0x00 || device_bindings[b].mac[1] == 0xE0))) {
              meter_resolve_canonical_mac_locked(device_bindings[b].mac, false, cand.mac);
              break;
            }
          }
        } else {
          meter_resolve_canonical_mac_locked(cand.mac, false, cand.mac);
        }
        if (is_recent_direct_disconnect(cand.mac, now_s)) continue;
        bool mac_exists = false;
        for (size_t m = 0; m < count; ++m) {
          if (memcmp(fresh[m].mac, cand.mac, 6) == 0) {
            if (fresh[m].ip == 0) {
              fresh[m].ip = cand.ip;
              mac_exists = true;
            } else if (fresh[m].ip == cand.ip) {
              mac_exists = true;
            }
            break;
          }
        }
        if (!mac_exists) {
          fresh[count++] = cand;
        }
      }
    }
  }

  // Compact fresh[] to exclude transparent Range Extender bridges that have no IP (ip == 0)
  active_client_t final_clients[MAX_CLIENTS] = {0};
  size_t final_count = 0;
  for (size_t i = 0; i < count && final_count < MAX_CLIENTS; ++i) {
    if (fresh[i].ip == 0) continue;
    final_clients[final_count++] = fresh[i];
  }

  memcpy(active_clients, final_clients, sizeof(final_clients));
  for (size_t i = 0; i < final_count; ++i) {
    meter_find_usage_locked(final_clients[i].mac, true);
    meter_find_policy_locked(final_clients[i].mac, true);
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
  uint8_t offer_dns = 0x02;
  ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
  ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip));
  esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                         ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
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

  // SoftAP station inactivity timeout: automatically deauth silent/out-of-range stations within 20s
  esp_wifi_set_inactive_time(WIFI_IF_AP, 20);

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

