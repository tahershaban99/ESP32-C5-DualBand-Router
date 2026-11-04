#include "meter.h"
#include "router_config.h"
#include "telegram_bot.h"
#include <string.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/etharp.h"

#include <time.h>
#include <stdio.h>

portMUX_TYPE data_mux = portMUX_INITIALIZER_UNLOCKED;

active_client_t active_clients[MAX_CLIENTS];
usage_record_t usage_records[MAX_RECORDS];
size_t usage_count = 0;
device_policy_t device_policies[MAX_RECORDS];
size_t policy_count = 0;
device_name_t device_names[MAX_RECORDS];
size_t name_count = 0;
device_speed_t device_speeds[MAX_RECORDS];
size_t speed_count = 0;
device_speed_bucket_t speed_buckets[MAX_RECORDS];
device_live_speed_t device_live_speeds[MAX_RECORDS];
device_binding_t device_bindings[MAX_RECORDS];
size_t binding_count = 0;

uint32_t total_cur_up_bps = 0;
uint32_t total_cur_down_bps = 0;

bool usage_dirty = false;
bool policy_dirty = false;
bool names_dirty = false;
bool speeds_dirty = false;
bool bindings_dirty = false;
bool device_catalog_dirty = false;

uint16_t meter_period_days = 30;
uint32_t meter_period_elapsed_s = 0;
bool meter_state_dirty = false;

uint64_t main_quota_bytes = 0;
char quota_start_date[12] = {0};
char quota_end_date[12] = {0};

int32_t reboot_countdown_s = -1;
volatile uint32_t pending_discovered_ip = 0;
static int last_fired_hour = -1;
static int last_fired_min = -1;

// Zero-Overhead Direct O(1) IP Lookup Table
dev_fast_entry_t *ip_lookup[256] = {NULL};
dev_fast_entry_t fast_device_pool[MAX_CLIENTS] = {0};

void meter_reset_reboot_fired_state(void) {
  last_fired_hour = -1;
  last_fired_min = -1;
}

// Howard Hinnant algorithm for civil date to epoch (pure integer, zero locks)
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int64_t)doe - 719468LL;
}

time_t meter_parse_date(const char *date_str, bool end_of_day) {
  if (!date_str || strlen(date_str) < 10) return 0;
  int y = 0, m = 0, d = 0;
  if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  if (y < 2020 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  int64_t days = days_from_civil(y, (unsigned)m, (unsigned)d);
  time_t ts = (time_t)(days * 86400LL);
  if (end_of_day) ts += 86399; // 23:59:59
  return ts;
}

void meter_rebuild_fast_lookup(void) {
  for (int i = 0; i < 256; ++i) {
    ip_lookup[i] = NULL;
  }
  for (size_t s = 0; s < MAX_CLIENTS; ++s) {
    if (active_clients[s].ip != 0 && (active_clients[s].aid != 0 || active_clients[s].last_seen_s > 0)) {
      uint32_t ip = active_clients[s].ip;
      uint8_t octet = ((uint8_t *)&ip)[3];
      if (octet >= 2 && octet <= 254) {
        fast_device_pool[s].ip = ip;
        memcpy(fast_device_pool[s].mac, active_clients[s].mac, 6);
        fast_device_pool[s].usage_idx = meter_find_usage_locked(active_clients[s].mac, true);
        fast_device_pool[s].policy_idx = meter_find_policy_locked(active_clients[s].mac, true);
        fast_device_pool[s].speed_idx = meter_find_speed_locked(active_clients[s].mac, true);
        fast_device_pool[s].blocked = (fast_device_pool[s].policy_idx >= 0) ? device_policies[fast_device_pool[s].policy_idx].blocked : false;
        fast_device_pool[s].gaming_mode = (fast_device_pool[s].policy_idx >= 0) ? device_policies[fast_device_pool[s].policy_idx].gaming_mode : false;
        fast_device_pool[s].last_seen_s = active_clients[s].last_seen_s;
        ip_lookup[octet] = &fast_device_pool[s];
      }
    } else {
      memset(&fast_device_pool[s], 0, sizeof(dev_fast_entry_t));
    }
  }
}

int meter_find_usage_locked(const uint8_t mac[6], bool create) {
  if (!valid_device_mac(mac)) return -1;
  for (size_t i = 0; i < usage_count; ++i) {
    if (memcmp(usage_records[i].mac, mac, 6) == 0) return (int)i;
  }
  if (!create || usage_count >= MAX_RECORDS) return -1;
  size_t i = usage_count++;
  memset(&usage_records[i], 0, sizeof(usage_records[i]));
  memset(&device_live_speeds[i], 0, sizeof(device_live_speeds[i]));
  memcpy(usage_records[i].mac, mac, 6);
  usage_dirty = true;
  device_catalog_dirty = true;
  return (int)i;
}

int meter_find_policy_locked(const uint8_t mac[6], bool create) {
  if (!valid_device_mac(mac)) return -1;
  for (size_t i = 0; i < policy_count; ++i) {
    if (memcmp(device_policies[i].mac, mac, 6) == 0) return (int)i;
  }
  if (!create || policy_count >= MAX_RECORDS) return -1;
  size_t i = policy_count++;
  memset(&device_policies[i], 0, sizeof(device_policies[i]));
  memcpy(device_policies[i].mac, mac, 6);
  policy_dirty = true;
  return (int)i;
}

int meter_find_name_locked(const uint8_t mac[6], bool create) {
  if (!valid_device_mac(mac)) return -1;
  for (size_t i = 0; i < name_count; ++i) {
    if (memcmp(device_names[i].mac, mac, 6) == 0) return (int)i;
  }
  if (!create || name_count >= MAX_RECORDS) return -1;
  size_t i = name_count++;
  memset(&device_names[i], 0, sizeof(device_names[i]));
  memcpy(device_names[i].mac, mac, 6);
  names_dirty = true;
  return (int)i;
}

int meter_find_speed_locked(const uint8_t mac[6], bool create) {
  if (!valid_device_mac(mac)) return -1;
  for (size_t i = 0; i < speed_count; ++i) {
    if (memcmp(device_speeds[i].mac, mac, 6) == 0) return (int)i;
  }
  if (!create || speed_count >= MAX_RECORDS) return -1;
  size_t i = speed_count++;
  memset(&device_speeds[i], 0, sizeof(device_speeds[i]));
  memset(&speed_buckets[i], 0, sizeof(device_speed_bucket_t));
  memcpy(device_speeds[i].mac, mac, 6);
  speeds_dirty = true;
  return (int)i;
}

int meter_find_binding_locked(const uint8_t mac[6], bool create) {
  if (!valid_device_mac(mac)) return -1;
  for (size_t i = 0; i < binding_count; ++i) {
    if (memcmp(device_bindings[i].mac, mac, 6) == 0) return (int)i;
  }
  if (!create || binding_count >= MAX_RECORDS) return -1;
  size_t i = binding_count++;
  memset(&device_bindings[i], 0, sizeof(device_bindings[i]));
  memcpy(device_bindings[i].mac, mac, 6);
  bindings_dirty = true;
  return (int)i;
}

void meter_set_main_quota(uint64_t quota_bytes, const char *start_date, const char *end_date) {
  portENTER_CRITICAL(&data_mux);
  main_quota_bytes = quota_bytes;
  if (start_date) {
    strncpy(quota_start_date, start_date, sizeof(quota_start_date) - 1);
    quota_start_date[sizeof(quota_start_date) - 1] = 0;
  }
  if (end_date) {
    strncpy(quota_end_date, end_date, sizeof(quota_end_date) - 1);
    quota_end_date[sizeof(quota_end_date) - 1] = 0;
  }
  meter_state_dirty = true;
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_period_days(uint16_t days) {
  if (days == 0) days = 30;
  portENTER_CRITICAL(&data_mux);
  meter_period_days = days;
  meter_state_dirty = true;
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_reset_all_usage(void) {
  portENTER_CRITICAL(&data_mux);
  for (size_t i = 0; i < usage_count; ++i) {
    usage_records[i].upload_bytes = 0;
    usage_records[i].download_bytes = 0;
    memset(&device_live_speeds[i], 0, sizeof(device_live_speeds[i]));
  }
  for (size_t i = 0; i < policy_count; ++i) {
    device_policies[i].blocked = false;
    device_policies[i].alert_flags = 0;
  }
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    fast_device_pool[i].blocked = false;
  }
  meter_period_elapsed_s = 0;
  usage_dirty = true;
  policy_dirty = true;
  meter_state_dirty = true;
  portEXIT_CRITICAL(&data_mux);

  // Reset Telegram alert threshold flags for fresh billing cycle!
  telegram_reset_alert_flags();

  meter_save_usage();
  ESP_LOGI(ROUTER_TAG, "All quota and meter counters have been reset. Telegram threshold flags cleared.");
}

void meter_set_device_policy(const uint8_t mac[6], uint64_t quota_bytes, bool blocked, bool gaming_mode) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_policy_locked(mac, true);
  if (idx >= 0) {
    if (device_policies[idx].quota_bytes != quota_bytes) {
      device_policies[idx].alert_flags = 0;
    }
    device_policies[idx].quota_bytes = quota_bytes;
    device_policies[idx].blocked = blocked;
    device_policies[idx].gaming_mode = gaming_mode;
    policy_dirty = true;
    for (size_t s = 0; s < MAX_CLIENTS; ++s) {
      if (memcmp(fast_device_pool[s].mac, mac, 6) == 0) {
        fast_device_pool[s].blocked = blocked;
        fast_device_pool[s].gaming_mode = gaming_mode;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_device_speed(const uint8_t mac[6], uint32_t upload_bps, uint32_t download_bps) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_speed_locked(mac, true);
  if (idx >= 0) {
    device_speeds[idx].upload_bps = upload_bps;
    device_speeds[idx].download_bps = download_bps;
    speeds_dirty = true;
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_device_name(const uint8_t mac[6], const char *name) {
  if (!valid_device_mac(mac) || !name) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_name_locked(mac, true);
  if (idx >= 0) {
    strncpy(device_names[idx].name, name, sizeof(device_names[idx].name) - 1);
    device_names[idx].name[sizeof(device_names[idx].name) - 1] = 0;
    names_dirty = true;
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_device_binding(const uint8_t mac[6], uint32_t ip) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_binding_locked(mac, true);
  if (idx >= 0) {
    device_bindings[idx].bound_ip = ip;
    bindings_dirty = true;
    device_catalog_dirty = true;
  }
  if (ip != 0) {
    for (size_t i = 0; i < binding_count; ++i) {
      if ((int)i != idx && device_bindings[i].bound_ip == ip) {
        device_bindings[i].bound_ip = 0;
        bindings_dirty = true;
      }
    }
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_device_blocked(const uint8_t mac[6], bool blocked) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_policy_locked(mac, true);
  if (idx >= 0) {
    device_policies[idx].blocked = blocked;
    policy_dirty = true;
    for (size_t s = 0; s < MAX_CLIENTS; ++s) {
      if (memcmp(fast_device_pool[s].mac, mac, 6) == 0) {
        fast_device_pool[s].blocked = blocked;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_device_gaming(const uint8_t mac[6], bool gaming) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_policy_locked(mac, true);
  if (idx >= 0) {
    device_policies[idx].gaming_mode = gaming;
    policy_dirty = true;
    for (size_t s = 0; s < MAX_CLIENTS; ++s) {
      if (memcmp(fast_device_pool[s].mac, mac, 6) == 0) {
        fast_device_pool[s].gaming_mode = gaming;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_add_device_quota(const uint8_t mac[6], int64_t add_bytes) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_policy_locked(mac, true);
  if (idx >= 0) {
    if (add_bytes <= 0) {
      device_policies[idx].quota_bytes = 0; // Unlimited
    } else {
      int u = meter_find_usage_locked(mac, false);
      uint64_t cur_used = (u >= 0) ? (usage_records[u].upload_bytes + usage_records[u].download_bytes) : 0;
      uint64_t base_q = device_policies[idx].quota_bytes;
      // If device had no quota or already exceeded its old quota, add on top of max(base_q, cur_used) so it immediately gets the full added amount
      if (base_q < cur_used) base_q = cur_used;
      device_policies[idx].quota_bytes = base_q + (uint64_t)add_bytes;
    }
    device_policies[idx].alert_flags = 0;
    device_policies[idx].blocked = false; // Auto-unblock immediately!
    policy_dirty = true;
    for (size_t s = 0; s < MAX_CLIENTS; ++s) {
      if (memcmp(fast_device_pool[s].mac, mac, 6) == 0) {
        fast_device_pool[s].blocked = false;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_device_alert_flag(const uint8_t mac[6], uint16_t mask) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_policy_locked(mac, false);
  if (idx >= 0) {
    device_policies[idx].alert_flags |= mask;
    policy_dirty = true;
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_set_device_telegram(const uint8_t mac[6], const char *chat_id) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_policy_locked(mac, true);
  if (idx >= 0) {
    if (chat_id && chat_id[0]) {
      strncpy(device_policies[idx].telegram_chat_id, chat_id, sizeof(device_policies[idx].telegram_chat_id) - 1);
      device_policies[idx].telegram_chat_id[sizeof(device_policies[idx].telegram_chat_id) - 1] = 0;
    } else {
      device_policies[idx].telegram_chat_id[0] = 0;
    }
    policy_dirty = true;
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

void meter_reset_device_usage(const uint8_t mac[6]) {
  if (!valid_device_mac(mac)) return;
  portENTER_CRITICAL(&data_mux);
  int idx = meter_find_usage_locked(mac, false);
  if (idx >= 0) {
    usage_records[idx].upload_bytes = 0;
    usage_records[idx].download_bytes = 0;
    memset(&device_live_speeds[idx], 0, sizeof(device_live_speeds[idx]));
    usage_dirty = true;
  }
  int p = meter_find_policy_locked(mac, false);
  if (p >= 0) {
    device_policies[p].alert_flags = 0;
    if (device_policies[p].blocked && device_policies[p].quota_bytes > 0) {
      device_policies[p].blocked = false;
      policy_dirty = true;
      for (size_t s = 0; s < MAX_CLIENTS; ++s) {
        if (memcmp(fast_device_pool[s].mac, mac, 6) == 0) {
          fast_device_pool[s].blocked = false;
          break;
        }
      }
    }
  }
  portEXIT_CRITICAL(&data_mux);
  meter_save_usage();
}

bool meter_delete_device(const uint8_t mac[6]) {
  if (!valid_device_mac(mac)) return false;
  bool changed = false;
  portENTER_CRITICAL(&data_mux);

  // 1. Remove from active_clients
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, mac, 6) == 0) {
      memset(&active_clients[i], 0, sizeof(active_clients[i]));
      break;
    }
  }

  // 2. Remove from usage_records
  for (size_t i = 0; i < usage_count; ++i) {
    if (memcmp(usage_records[i].mac, mac, 6) == 0) {
      for (size_t j = i; j + 1 < usage_count; ++j) {
        usage_records[j] = usage_records[j + 1];
        device_live_speeds[j] = device_live_speeds[j + 1];
      }
      --usage_count;
      memset(&usage_records[usage_count], 0, sizeof(usage_record_t));
      memset(&device_live_speeds[usage_count], 0, sizeof(device_live_speed_t));
      usage_dirty = true;
      changed = true;
      break;
    }
  }

  // 3. Remove from device_policies
  for (size_t i = 0; i < policy_count; ++i) {
    if (memcmp(device_policies[i].mac, mac, 6) == 0) {
      for (size_t j = i; j + 1 < policy_count; ++j) {
        device_policies[j] = device_policies[j + 1];
      }
      --policy_count;
      memset(&device_policies[policy_count], 0, sizeof(device_policy_t));
      policy_dirty = true;
      changed = true;
      break;
    }
  }

  // 4. Remove from device_names
  for (size_t i = 0; i < name_count; ++i) {
    if (memcmp(device_names[i].mac, mac, 6) == 0) {
      for (size_t j = i; j + 1 < name_count; ++j) {
        device_names[j] = device_names[j + 1];
      }
      --name_count;
      memset(&device_names[name_count], 0, sizeof(device_name_t));
      names_dirty = true;
      changed = true;
      break;
    }
  }

  // 5. Remove from device_speeds
  for (size_t i = 0; i < speed_count; ++i) {
    if (memcmp(device_speeds[i].mac, mac, 6) == 0) {
      for (size_t j = i; j + 1 < speed_count; ++j) {
        device_speeds[j] = device_speeds[j + 1];
        speed_buckets[j] = speed_buckets[j + 1];
      }
      --speed_count;
      memset(&device_speeds[speed_count], 0, sizeof(device_speed_t));
      memset(&speed_buckets[speed_count], 0, sizeof(device_speed_bucket_t));
      speeds_dirty = true;
      changed = true;
      break;
    }
  }

  // 6. Remove from device_bindings
  for (size_t i = 0; i < binding_count; ++i) {
    if (memcmp(device_bindings[i].mac, mac, 6) == 0) {
      for (size_t j = i; j + 1 < binding_count; ++j) {
        device_bindings[j] = device_bindings[j + 1];
      }
      --binding_count;
      memset(&device_bindings[binding_count], 0, sizeof(device_binding_t));
      bindings_dirty = true;
      changed = true;
      break;
    }
  }

  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);

  if (changed) {
    meter_save_usage();
  }
  return changed;
}

void meter_sync_device_on_dhcp(const uint8_t physical_mac[6], uint32_t ip) {
  if (!valid_device_mac(physical_mac) || ip == 0) return;

  uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  bool need_save = false;
  portENTER_CRITICAL(&data_mux);

  uint8_t target_mac[6];
  memcpy(target_mac, physical_mac, 6);

  // If a Range Extender rewrote DHCP chaddr to its own MAC and that MAC already
  // owns a different active IP, synthesize a per-IP virtual MAC so each client
  // behind the extender remains an independent device.
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, physical_mac, 6) == 0 &&
        active_clients[i].aid != 0 && active_clients[i].ip != 0 &&
        active_clients[i].ip != ip) {
      target_mac[0] = 0x02;
      target_mac[1] = 0xE0;
      target_mac[2] = 192;
      target_mac[3] = 168;
      target_mac[4] = 4;
      target_mac[5] = ((const uint8_t *)&ip)[3];
      break;
    }
  }

  // If this IP previously used a fallback 02:E0:C0:A8:04:XX MAC before DHCP
  // revealed the true hardware MAC, migrate its accumulated bytes seamlessly.
  if (!(target_mac[0] == 0x02 && target_mac[1] == 0xE0)) {
    uint8_t fallback_mac[6] = {0x02, 0xE0, 192, 168, 4, ((const uint8_t *)&ip)[3]};
    int fb_u = meter_find_usage_locked(fallback_mac, false);
    int real_u = meter_find_usage_locked(target_mac, true);
    if (fb_u >= 0 && real_u >= 0 && fb_u != real_u) {
      usage_records[real_u].upload_bytes += usage_records[fb_u].upload_bytes;
      usage_records[real_u].download_bytes += usage_records[fb_u].download_bytes;
      for (size_t j = (size_t)fb_u; j + 1 < usage_count; ++j) {
        usage_records[j] = usage_records[j + 1];
        device_live_speeds[j] = device_live_speeds[j + 1];
      }
      --usage_count;
      memset(&usage_records[usage_count], 0, sizeof(usage_record_t));
      memset(&device_live_speeds[usage_count], 0, sizeof(device_live_speed_t));
      usage_dirty = true;
      need_save = true;
    }
  } else {
    meter_find_usage_locked(target_mac, true);
  }
  meter_find_policy_locked(target_mac, true);

  // Update MAC-to-IP binding and clear any stale binding that pointed to this IP
  int b_idx = -1;
  for (size_t b = 0; b < binding_count; ++b) {
    if (memcmp(device_bindings[b].mac, target_mac, 6) == 0) {
      b_idx = (int)b;
    } else if (device_bindings[b].bound_ip == ip) {
      device_bindings[b].bound_ip = 0;
      bindings_dirty = true;
      need_save = true;
    }
  }
  if (b_idx >= 0) {
    if (device_bindings[b_idx].bound_ip != ip) {
      device_bindings[b_idx].bound_ip = ip;
      bindings_dirty = true;
      need_save = true;
    }
  } else if (binding_count < MAX_RECORDS) {
    memcpy(device_bindings[binding_count].mac, target_mac, 6);
    device_bindings[binding_count].bound_ip = ip;
    ++binding_count;
    bindings_dirty = true;
    need_save = true;
  }

  // Clear this IP from any other slot in active_clients
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (active_clients[i].ip == ip && memcmp(active_clients[i].mac, target_mac, 6) != 0) {
      if (active_clients[i].aid == 0xFF) {
        memset(&active_clients[i], 0, sizeof(active_clients[i]));
      } else {
        active_clients[i].ip = 0;
      }
    }
  }

  // Map to active clients slot (or allocate a bridged 0xFF slot for Range Extender clients!)
  bool mapped = false;
  size_t free_slot = MAX_CLIENTS;
  size_t oldest_bridged = MAX_CLIENTS;
  uint32_t oldest_seen = UINT32_MAX;

  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, target_mac, 6) == 0) {
      active_clients[i].ip = ip;
      active_clients[i].last_seen_s = now_s;
      if (active_clients[i].aid == 0) active_clients[i].aid = 0xFF;
      if (active_clients[i].rssi == 0) active_clients[i].rssi = -55;
      mapped = true;
      break;
    }
    if (free_slot == MAX_CLIENTS && !valid_device_mac(active_clients[i].mac)) {
      free_slot = i;
    }
    if (active_clients[i].aid == 0xFF && active_clients[i].last_seen_s < oldest_seen) {
      oldest_seen = active_clients[i].last_seen_s;
      oldest_bridged = i;
    }
  }

  if (!mapped) {
    size_t target_slot = (free_slot < MAX_CLIENTS) ? free_slot : oldest_bridged;
    if (target_slot < MAX_CLIENTS) {
      memcpy(active_clients[target_slot].mac, target_mac, 6);
      active_clients[target_slot].ip = ip;
      active_clients[target_slot].aid = 0xFF; // Bridged / Range Extender client
      active_clients[target_slot].rssi = -55;
      active_clients[target_slot].last_seen_s = now_s;
    }
  }

  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);

  if (need_save) {
    meter_save_usage();
  }
}

static inline uint32_t ipv4_total_len(const uint8_t *header, size_t avail) {
  if (avail < 20 || (header[0] >> 4) != 4) return 0;
  uint8_t header_len = (header[0] & 0x0f) * 4;
  uint32_t total = ((uint32_t)header[2] << 8) | header[3];
  if (header_len < 20 || total < header_len || total > 65535) return 0;
  return total;
}

// -----------------------------------------------------------------------------
// ZERO-OVERHEAD FAST PACKET HOOK:
// In IRAM, O(1) direct lookup ip_lookup[256], zero floating-point, zero malloc, zero locks!
// -----------------------------------------------------------------------------
int meter_ip4_canforward(struct pbuf *packet, uint32_t destination_host_order) {
  (void)destination_host_order;
  if (packet == NULL) return -1;
  uint8_t ip[20];
  if (pbuf_copy_partial(packet, ip, sizeof(ip), 0) != sizeof(ip)) return -1;
  uint32_t bytes = ipv4_total_len(ip, packet->tot_len);
  if (bytes == 0) return -1;

  uint32_t source_ip, destination_ip;
  memcpy(&source_ip, ip + 12, sizeof(source_ip));
  memcpy(&destination_ip, ip + 16, sizeof(destination_ip));
  uint32_t routed_destination_ip = lwip_htonl(destination_host_order);

  uint32_t target_local_ip = 0;
  bool is_upload = false;
  if ((source_ip & 0x00FFFFFF) == 0x0004A8C0 && (source_ip >> 24) != 1) {
    target_local_ip = source_ip;
    is_upload = true;
  } else if ((routed_destination_ip & 0x00FFFFFF) == 0x0004A8C0 && (routed_destination_ip >> 24) != 1) {
    target_local_ip = routed_destination_ip;
  } else if ((destination_ip & 0x00FFFFFF) == 0x0004A8C0 && (destination_ip >> 24) != 1) {
    target_local_ip = destination_ip;
  }
  if (target_local_ip == 0) return -1;

  uint8_t octet = ((uint8_t *)&target_local_ip)[3];
  dev_fast_entry_t *dev = ip_lookup[octet];
  if (dev == NULL) {
    pending_discovered_ip = target_local_ip;
    return -1; // Fast-forward unmapped packet and queue zero-overhead L3 discovery
  }

  int64_t now_us = esp_timer_get_time();
  dev->last_seen_s = (uint32_t)(now_us / 1000000ULL);

  // 1-cycle O(1) Block Check
  if (dev->blocked) {
    return 0; // DROP
  }

  // Smart QoS Fast-Track: ICMP (Ping), small UDP packets (<= 450B), small TCP ACKs (<= 70B), or VIP Gaming
  uint8_t ip_proto = ip[9];
  bool is_gaming_packet = (ip_proto == 1) ||
                          (ip_proto == 17 && bytes <= 450) ||
                          (ip_proto == 6 && bytes <= 70);
  bool is_priority = is_gaming_packet || dev->gaming_mode;

  // Rate Limiting (Token Bucket with pure integer microsecond arithmetic)
  if (!is_priority && dev->speed_idx >= 0 && (size_t)dev->speed_idx < speed_count) {
    int s_idx = dev->speed_idx;
    uint32_t limit_bps = is_upload ? device_speeds[s_idx].upload_bps : device_speeds[s_idx].download_bps;
    if (limit_bps > 0) {
      uint64_t rate_Bps = ((uint64_t)limit_bps + 7U) >> 3;
      uint64_t capacity = rate_Bps >> 2; // 250ms burst
      if (capacity < 1500U) capacity = 1500U;
      uint64_t *tokens = is_upload ? &speed_buckets[s_idx].upload_tokens : &speed_buckets[s_idx].download_tokens;
      int64_t *updated_us = is_upload ? &speed_buckets[s_idx].upload_updated_us : &speed_buckets[s_idx].download_updated_us;
      if (*updated_us == 0) {
        *tokens = capacity;
        *updated_us = now_us;
      } else if (now_us > *updated_us) {
        int64_t elapsed_us = now_us - *updated_us;
        if (elapsed_us > 1000000) elapsed_us = 1000000;
        uint64_t refill = (rate_Bps * (uint64_t)elapsed_us) / 1000000ULL;
        uint64_t t = *tokens + refill;
        *tokens = (t > capacity) ? capacity : t;
        *updated_us = now_us;
      }
      if (*tokens < bytes) {
        return 0; // DROP: Token bucket rate limit exceeded
      }
      *tokens -= bytes;
    }
  }

  // Byte Accounting & Quota Enforcement (Pure Integer Math)
  if (dev->usage_idx >= 0 && (size_t)dev->usage_idx < usage_count) {
    uint64_t metered_bytes = bytes;
    if (router_meter_scale_pct != 100) {
      metered_bytes = ((uint64_t)bytes * (uint64_t)router_meter_scale_pct) / 100ULL;
    }
    if (is_upload) {
      usage_records[dev->usage_idx].upload_bytes += metered_bytes;
    } else {
      usage_records[dev->usage_idx].download_bytes += metered_bytes;
    }
    usage_dirty = true;

    // Check device quota limit
    if (dev->policy_idx >= 0 && (size_t)dev->policy_idx < policy_count) {
      uint64_t quota = device_policies[dev->policy_idx].quota_bytes;
      if (quota > 0) {
        uint64_t tot = usage_records[dev->usage_idx].upload_bytes + usage_records[dev->usage_idx].download_bytes;
        if (tot >= quota) {
          device_policies[dev->policy_idx].blocked = true;
          dev->blocked = true;
          policy_dirty = true;
          return 0; // DROP: Device quota exceeded
        }
      }
    }
  }

  return -1; // Forward packet
}

void meter_tick_second(uint32_t elapsed_s) {
  if (elapsed_s == 0) elapsed_s = 1;
  uint32_t agg_up_bps = 0;
  uint32_t agg_down_bps = 0;
  uint64_t grand_total_used = 0;

  portENTER_CRITICAL(&data_mux);
  meter_period_elapsed_s += elapsed_s;
  meter_state_dirty = true;

  for (size_t i = 0; i < usage_count; ++i) {
    uint64_t cur_up = usage_records[i].upload_bytes;
    uint64_t cur_down = usage_records[i].download_bytes;
    grand_total_used += (cur_up + cur_down);

    if (cur_up >= device_live_speeds[i].last_up_bytes) {
      uint64_t diff = cur_up - device_live_speeds[i].last_up_bytes;
      uint32_t inst_bps = (uint32_t)((diff * 8ULL) / elapsed_s);
      device_live_speeds[i].cur_up_bps = (device_live_speeds[i].cur_up_bps == 0) ? inst_bps : ((inst_bps * 7 + device_live_speeds[i].cur_up_bps * 3) / 10);
    } else {
      device_live_speeds[i].cur_up_bps = 0;
    }
    device_live_speeds[i].last_up_bytes = cur_up;

    if (cur_down >= device_live_speeds[i].last_down_bytes) {
      uint64_t diff = cur_down - device_live_speeds[i].last_down_bytes;
      uint32_t inst_bps = (uint32_t)((diff * 8ULL) / elapsed_s);
      device_live_speeds[i].cur_down_bps = (device_live_speeds[i].cur_down_bps == 0) ? inst_bps : ((inst_bps * 7 + device_live_speeds[i].cur_down_bps * 3) / 10);
    } else {
      device_live_speeds[i].cur_down_bps = 0;
    }
    device_live_speeds[i].last_down_bytes = cur_down;

    agg_up_bps += device_live_speeds[i].cur_up_bps;
    agg_down_bps += device_live_speeds[i].cur_down_bps;
  }
  total_cur_up_bps = agg_up_bps;
  total_cur_down_bps = agg_down_bps;

  // Sync live packet timestamps from fast_device_pool and age out inactive bridged devices
  uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (active_clients[k].ip != 0 && fast_device_pool[k].ip == active_clients[k].ip) {
      if (fast_device_pool[k].last_seen_s > active_clients[k].last_seen_s) {
        active_clients[k].last_seen_s = fast_device_pool[k].last_seen_s;
      }
    }
    if (active_clients[k].aid == 0xFF && active_clients[k].ip != 0) {
      if (now_s > active_clients[k].last_seen_s &&
          (now_s - active_clients[k].last_seen_s) > BRIDGED_CLIENT_TIMEOUT_S) {
        uint8_t oct = ((uint8_t *)&active_clients[k].ip)[3];
        ip_lookup[oct] = NULL;
        memset(&active_clients[k], 0, sizeof(active_clients[k]));
        memset(&fast_device_pool[k], 0, sizeof(fast_device_pool[k]));
      }
    }
  }

  // Auto-discover active Range Extender / bridged clients from live L3 traffic
  uint32_t disc_ip = pending_discovered_ip;
  if (disc_ip != 0) {
    pending_discovered_ip = 0;
    uint8_t disc_oct = ((const uint8_t *)&disc_ip)[3];
    if (disc_oct >= 2 && disc_oct <= 254 && ip_lookup[disc_oct] == NULL) {
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
      size_t slot = MAX_CLIENTS;
      for (size_t k = 0; k < MAX_CLIENTS; ++k) {
        if (memcmp(active_clients[k].mac, owner_mac, 6) == 0 &&
            (active_clients[k].ip == 0 || active_clients[k].ip == disc_ip)) {
          slot = k;
          break;
        }
        if (slot == MAX_CLIENTS && !valid_device_mac(active_clients[k].mac)) {
          slot = k;
        }
      }
      if (slot < MAX_CLIENTS) {
        memcpy(active_clients[slot].mac, owner_mac, 6);
        active_clients[slot].ip = disc_ip;
        if (active_clients[slot].aid == 0) active_clients[slot].aid = 0xFF;
        if (active_clients[slot].rssi == 0) active_clients[slot].rssi = -58;
        active_clients[slot].last_seen_s = now_s;
        meter_find_usage_locked(owner_mac, true);
        meter_find_policy_locked(owner_mac, true);
        meter_rebuild_fast_lookup();
      }
    }
  }
  portEXIT_CRITICAL(&data_mux);

  // Check Quick Countdown Reboot
  if (reboot_countdown_s > 0) {
    reboot_countdown_s -= elapsed_s;
    if (reboot_countdown_s <= 0) {
      ESP_LOGW(ROUTER_TAG, "Countdown reboot timer expired! Restarting ESP32...");
      meter_save_usage();
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
    }
  }

  // Check Daily Scheduled Auto-Reboot (NTP Synchronized)
  time_t now = time(NULL);
  if (auto_reboot_enabled && now > 1700000000) { // Valid NTP sync
    time_t local_now = now + ((int64_t)auto_reboot_tz_offset * 3600LL);
    struct tm tm_info;
    gmtime_r(&local_now, &tm_info);
    if (tm_info.tm_hour == auto_reboot_hour && tm_info.tm_min == auto_reboot_min) {
      if (tm_info.tm_hour != last_fired_hour || tm_info.tm_min != last_fired_min) {
        last_fired_hour = tm_info.tm_hour;
        last_fired_min = tm_info.tm_min;
        ESP_LOGW(ROUTER_TAG, "Daily Scheduled Auto-Reboot fired at %02d:%02d (TZ UTC%+d). Restarting ESP32...",
                 tm_info.tm_hour, tm_info.tm_min, auto_reboot_tz_offset);
        meter_save_usage();
        config_set_scheduled_reboot_flag(true);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      }
    } else {
      last_fired_hour = -1;
      last_fired_min = -1;
    }
  } else if (auto_reboot_enabled && now <= 1700000000) {
    // Unsynced NTP: safely postpone reboot by 1 hour to prevent rebooting at wrong time
    static uint32_t last_unsync_warn_s = 0;
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (now_s - last_unsync_warn_s >= 3600) {
      last_unsync_warn_s = now_s;
      ESP_LOGW(ROUTER_TAG, "Auto-reboot postponed: NTP time unsynced. Retrying in 1 hour when connection is active...");
    }
  }

}

void meter_load_usage(void) {
  meter_period_days = 30;
  meter_period_elapsed_s = 0;
  nvs_handle_t nvs;
  if (nvs_open("meter", NVS_READONLY, &nvs) == ESP_OK) {
    size_t size = 0;
    if (nvs_get_blob(nvs, "records", NULL, &size) == ESP_OK && size > 0 && size % sizeof(usage_record_t) == 0) {
      if (size <= sizeof(usage_records)) {
        if (nvs_get_blob(nvs, "records", usage_records, &size) == ESP_OK) {
          usage_count = size / sizeof(usage_record_t);
        }
      } else {
        void *tmp = malloc(size);
        if (tmp && nvs_get_blob(nvs, "records", tmp, &size) == ESP_OK) {
          usage_count = MAX_RECORDS;
          memcpy(usage_records, tmp, sizeof(usage_records));
          usage_dirty = true;
        }
        if (tmp) free(tmp);
      }
    }

    size = 0;
    if (nvs_get_blob(nvs, "policies", NULL, &size) == ESP_OK && size > 0) {
      if (size % sizeof(device_policy_t) == 0) {
        if (size <= sizeof(device_policies)) {
          if (nvs_get_blob(nvs, "policies", device_policies, &size) == ESP_OK) {
            policy_count = size / sizeof(device_policy_t);
          }
        } else {
          void *tmp = malloc(size);
          if (tmp && nvs_get_blob(nvs, "policies", tmp, &size) == ESP_OK) {
            policy_count = MAX_RECORDS;
            memcpy(device_policies, tmp, sizeof(device_policies));
            policy_dirty = true;
          }
          if (tmp) free(tmp);
        }
      }
    }

    size = 0;
    if (nvs_get_blob(nvs, "names", NULL, &size) == ESP_OK && size > 0 && size % sizeof(device_name_t) == 0) {
      if (size <= sizeof(device_names)) {
        if (nvs_get_blob(nvs, "names", device_names, &size) == ESP_OK) {
          name_count = size / sizeof(device_name_t);
        }
      } else {
        void *tmp = malloc(size);
        if (tmp && nvs_get_blob(nvs, "names", tmp, &size) == ESP_OK) {
          name_count = MAX_RECORDS;
          memcpy(device_names, tmp, sizeof(device_names));
          names_dirty = true;
        }
        if (tmp) free(tmp);
      }
    }

    size = 0;
    if (nvs_get_blob(nvs, "speeds", NULL, &size) == ESP_OK && size > 0 && size % sizeof(device_speed_t) == 0) {
      if (size <= sizeof(device_speeds)) {
        if (nvs_get_blob(nvs, "speeds", device_speeds, &size) == ESP_OK) {
          speed_count = size / sizeof(device_speed_t);
        }
      } else {
        void *tmp = malloc(size);
        if (tmp && nvs_get_blob(nvs, "speeds", tmp, &size) == ESP_OK) {
          speed_count = MAX_RECORDS;
          memcpy(device_speeds, tmp, sizeof(device_speeds));
          speeds_dirty = true;
        }
        if (tmp) free(tmp);
      }
    }

    size = 0;
    if (nvs_get_blob(nvs, "bindings", NULL, &size) == ESP_OK && size > 0 && size % sizeof(device_binding_t) == 0) {
      if (size <= sizeof(device_bindings)) {
        if (nvs_get_blob(nvs, "bindings", device_bindings, &size) == ESP_OK) {
          binding_count = size / sizeof(device_binding_t);
        }
      } else {
        void *tmp = malloc(size);
        if (tmp && nvs_get_blob(nvs, "bindings", tmp, &size) == ESP_OK) {
          binding_count = MAX_RECORDS;
          memcpy(device_bindings, tmp, sizeof(device_bindings));
          bindings_dirty = true;
        }
        if (tmp) free(tmp);
      }
    }

    nvs_get_u16(nvs, "period_days", &meter_period_days);
    nvs_get_u32(nvs, "elapsed_s", &meter_period_elapsed_s);
    nvs_get_u64(nvs, "main_quota", &main_quota_bytes);

    size = sizeof(quota_start_date);
    nvs_get_str(nvs, "q_start", quota_start_date, &size);
    size = sizeof(quota_end_date);
    nvs_get_str(nvs, "q_end", quota_end_date, &size);

    nvs_close(nvs);

    // Migrate legacy 02:00:ip bytes to physical MACs and purge 02:00: records
    for (size_t k = 0; k < usage_count; ++k) {
      if (usage_records[k].mac[0] == 0x02 && usage_records[k].mac[1] == 0x00) {
        uint32_t rip = 0;
        memcpy(&rip, usage_records[k].mac + 2, 4);
        if (rip != 0) {
          for (size_t b = 0; b < binding_count; ++b) {
            if (device_bindings[b].bound_ip == rip && valid_device_mac(device_bindings[b].mac) &&
                !(device_bindings[b].mac[0] == 0x02 && device_bindings[b].mac[1] == 0x00)) {
              int pu = meter_find_usage_locked(device_bindings[b].mac, true);
              if (pu >= 0) {
                usage_records[pu].upload_bytes += usage_records[k].upload_bytes;
                usage_records[pu].download_bytes += usage_records[k].download_bytes;
                usage_dirty = true;
              }
              break;
            }
          }
        }
      }
    }

    size_t clean_u = 0;
    for (size_t k = 0; k < usage_count; ++k) {
      if (usage_records[k].mac[0] == 0x02 && usage_records[k].mac[1] == 0x00) continue;
      if (clean_u != k) usage_records[clean_u] = usage_records[k];
      clean_u++;
    }
    usage_count = clean_u;

    size_t clean_p = 0;
    for (size_t k = 0; k < policy_count; ++k) {
      if (device_policies[k].mac[0] == 0x02 && device_policies[k].mac[1] == 0x00) continue;
      if (clean_p != k) device_policies[clean_p] = device_policies[k];
      clean_p++;
    }
    policy_count = clean_p;

    size_t clean_s = 0;
    for (size_t k = 0; k < speed_count; ++k) {
      if (device_speeds[k].mac[0] == 0x02 && device_speeds[k].mac[1] == 0x00) continue;
      if (clean_s != k) device_speeds[clean_s] = device_speeds[k];
      clean_s++;
    }
    speed_count = clean_s;

    size_t clean_n = 0;
    for (size_t k = 0; k < name_count; ++k) {
      if (device_names[k].mac[0] == 0x02 && device_names[k].mac[1] == 0x00) continue;
      if (clean_n != k) device_names[clean_n] = device_names[k];
      clean_n++;
    }
    name_count = clean_n;

    size_t clean_b = 0;
    for (size_t k = 0; k < binding_count; ++k) {
      if (device_bindings[k].mac[0] == 0x02 && device_bindings[k].mac[1] == 0x00) continue;
      if (clean_b != k) device_bindings[clean_b] = device_bindings[k];
      clean_b++;
    }
    binding_count = clean_b;
  }

  meter_rebuild_fast_lookup();
  ESP_LOGI(ROUTER_TAG, "Loaded usage records: %zu devices, main quota: %llu bytes", usage_count, (unsigned long long)main_quota_bytes);
}

void meter_save_usage(void) {
  if (!usage_dirty && !policy_dirty && !names_dirty && !speeds_dirty && !bindings_dirty && !meter_state_dirty) {
    return;
  }

  bool do_usage = false, do_policy = false, do_names = false, do_speeds = false, do_bindings = false, do_state = false;
  size_t u_cnt = 0, p_cnt = 0, n_cnt = 0, s_cnt = 0, b_cnt = 0;
  uint16_t snap_period_days = 0;
  uint32_t snap_elapsed_s = 0;
  uint64_t snap_main_quota = 0;
  char snap_q_start[16] = {0}, snap_q_end[16] = {0};

  portENTER_CRITICAL(&data_mux);
  do_usage = usage_dirty;       u_cnt = usage_count;
  do_policy = policy_dirty;     p_cnt = policy_count;
  do_names = names_dirty;       n_cnt = name_count;
  do_speeds = speeds_dirty;     s_cnt = speed_count;
  do_bindings = bindings_dirty; b_cnt = binding_count;
  do_state = meter_state_dirty;
  portEXIT_CRITICAL(&data_mux);

  usage_record_t *snap_usage = (do_usage && u_cnt > 0) ? malloc(u_cnt * sizeof(usage_record_t)) : NULL;
  device_policy_t *snap_policies = (do_policy && p_cnt > 0) ? malloc(p_cnt * sizeof(device_policy_t)) : NULL;
  device_name_t *snap_names = (do_names && n_cnt > 0) ? malloc(n_cnt * sizeof(device_name_t)) : NULL;
  device_speed_t *snap_speeds = (do_speeds && s_cnt > 0) ? malloc(s_cnt * sizeof(device_speed_t)) : NULL;
  device_binding_t *snap_bindings = (do_bindings && b_cnt > 0) ? malloc(b_cnt * sizeof(device_binding_t)) : NULL;

  portENTER_CRITICAL(&data_mux);
  if (snap_usage && usage_count <= u_cnt) {
    u_cnt = usage_count;
    memcpy(snap_usage, usage_records, u_cnt * sizeof(usage_record_t));
    usage_dirty = false;
  } else {
    do_usage = false;
  }
  if (snap_policies && policy_count <= p_cnt) {
    p_cnt = policy_count;
    memcpy(snap_policies, device_policies, p_cnt * sizeof(device_policy_t));
    policy_dirty = false;
  } else {
    do_policy = false;
  }
  if (snap_names && name_count <= n_cnt) {
    n_cnt = name_count;
    memcpy(snap_names, device_names, n_cnt * sizeof(device_name_t));
    names_dirty = false;
  } else {
    do_names = false;
  }
  if (snap_speeds && speed_count <= s_cnt) {
    s_cnt = speed_count;
    memcpy(snap_speeds, device_speeds, s_cnt * sizeof(device_speed_t));
    speeds_dirty = false;
  } else {
    do_speeds = false;
  }
  if (snap_bindings && binding_count <= b_cnt) {
    b_cnt = binding_count;
    memcpy(snap_bindings, device_bindings, b_cnt * sizeof(device_binding_t));
    bindings_dirty = false;
  } else {
    do_bindings = false;
  }
  if (meter_state_dirty) {
    snap_period_days = meter_period_days;
    snap_elapsed_s = meter_period_elapsed_s;
    snap_main_quota = main_quota_bytes;
    strncpy(snap_q_start, quota_start_date, sizeof(snap_q_start) - 1);
    strncpy(snap_q_end, quota_end_date, sizeof(snap_q_end) - 1);
    meter_state_dirty = false;
    do_state = true;
  }
  device_catalog_dirty = false;
  portEXIT_CRITICAL(&data_mux);

  nvs_handle_t nvs;
  if (nvs_open("meter", NVS_READWRITE, &nvs) == ESP_OK) {
    if (do_usage && snap_usage && u_cnt > 0) nvs_set_blob(nvs, "records", snap_usage, u_cnt * sizeof(usage_record_t));
    if (do_policy && snap_policies && p_cnt > 0) nvs_set_blob(nvs, "policies", snap_policies, p_cnt * sizeof(device_policy_t));
    if (do_names && snap_names && n_cnt > 0) nvs_set_blob(nvs, "names", snap_names, n_cnt * sizeof(device_name_t));
    if (do_speeds && snap_speeds && s_cnt > 0) nvs_set_blob(nvs, "speeds", snap_speeds, s_cnt * sizeof(device_speed_t));
    if (do_bindings && snap_bindings && b_cnt > 0) nvs_set_blob(nvs, "bindings", snap_bindings, b_cnt * sizeof(device_binding_t));
    if (do_state) {
      nvs_set_u16(nvs, "period_days", snap_period_days);
      nvs_set_u32(nvs, "elapsed_s", snap_elapsed_s);
      nvs_set_u64(nvs, "main_quota", snap_main_quota);
      nvs_set_str(nvs, "q_start", snap_q_start);
      nvs_set_str(nvs, "q_end", snap_q_end);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
  }

  if (snap_usage) free(snap_usage);
  if (snap_policies) free(snap_policies);
  if (snap_names) free(snap_names);
  if (snap_speeds) free(snap_speeds);
  if (snap_bindings) free(snap_bindings);
}

