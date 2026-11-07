#include "web_server.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include <netinet/in.h>
#include <sys/socket.h>

#include "meter.h"
#include "ota_updater.h"
#include "router_config.h"
#include "router_globals.h"
#include "telegram_bot.h"
#include "web_page_gz.h"
#include "wifi_manager.h"

httpd_handle_t web_server = NULL;

#define MAX_ADMIN_SESSIONS 8
typedef struct {
  char token[65];
  int64_t expiry_us;
  bool valid;
} admin_session_t;

static admin_session_t s_admin_sessions[MAX_ADMIN_SESSIONS] = {0};
static uint32_t failed_login_attempts = 0;
static int64_t lockout_until_us = 0;

#define SESSION_TTL_US (3600ULL * 24ULL * 1000000ULL)
#define LOCKOUT_DURATION_US (60ULL * 1000000ULL)
#define MAX_FAILED_ATTEMPTS 5

static void url_decode(char *s) {
  char *out = s;
  while (*s) {
    if (*s == '+') {
      *out++ = ' ';
      ++s;
    } else if (*s == '%' && s[1] && s[2]) {
      char hex[3] = {s[1], s[2], 0};
      *out++ = (char)strtol(hex, NULL, 16);
      s += 3;
    } else
      *out++ = *s++;
  }
  *out = 0;
}

static void form_value(char *body, const char *key, char *dest,
                       size_t capacity) {
  char search[48];
  snprintf(search, sizeof(search), "%s=", key);
  char *p = strstr(body, search);
  if (!p) {
    dest[0] = 0;
    return;
  }
  p += strlen(search);
  char *end = strchr(p, '&');
  size_t n = end ? (size_t)(end - p) : strlen(p);
  if (n >= capacity)
    n = capacity - 1;
  memcpy(dest, p, n);
  dest[n] = 0;
  url_decode(dest);
}

static void format_mac(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

static bool parse_mac_address(const char *text, uint8_t mac[6]) {
  if (!text || strlen(text) != 17)
    return false;
  for (size_t i = 0; i < 6; ++i) {
    char pair[3] = {text[i * 3], text[i * 3 + 1], 0};
    char *end = NULL;
    unsigned long value = strtoul(pair, &end, 16);
    if (end != pair + 2 || value > 255 ||
        (i < 5 && text[i * 3 + 2] != ':' && text[i * 3 + 2] != '-'))
      return false;
    mac[i] = (uint8_t)value;
  }
  return valid_device_mac(mac);
}

static size_t append_json_string(char *dst, size_t capacity, size_t used,
                                 const char *src) {
  if (used + 2 >= capacity)
    return used;
  dst[used++] = '"';
  for (const unsigned char *p = (const unsigned char *)src;
       *p && used + 3 < capacity; ++p) {
    if (*p == '"' || *p == '\\')
      dst[used++] = '\\';
    dst[used++] = (*p < 0x20) ? '_' : (char)*p;
  }
  dst[used++] = '"';
  dst[used] = 0;
  return used;
}

static esp_err_t read_small_form(httpd_req_t *req, char *body,
                                 size_t capacity) {
  if (req->content_len <= 0 || req->content_len >= capacity) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    return ESP_FAIL;
  }
  int received = 0;
  while (received < req->content_len) {
    int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form read failed");
      return ESP_FAIL;
    }
    received += n;
  }
  body[received] = 0;
  return ESP_OK;
}

static bool constant_time_eq(const char *a, const char *b) {
  if (!a || !b)
    return false;
  size_t len_a = strlen(a);
  size_t len_b = strlen(b);
  if (len_a != len_b)
    return false;
  unsigned char result = 0;
  for (size_t i = 0; i < len_a; ++i) {
    result |= (unsigned char)(a[i] ^ b[i]);
  }
  return result == 0;
}

static bool is_token_valid(const char *tok) {
  if (!tok || tok[0] == 0)
    return false;
  int64_t now_us = esp_timer_get_time();
  for (size_t i = 0; i < MAX_ADMIN_SESSIONS; ++i) {
    if (s_admin_sessions[i].valid) {
      if (now_us > s_admin_sessions[i].expiry_us) {
        s_admin_sessions[i].valid = false;
        continue;
      }
      if (constant_time_eq(tok, s_admin_sessions[i].token)) {
        return true;
      }
    }
  }
  return false;
}

static void add_admin_session(const char *tok, int64_t expiry_us) {
  if (!tok || tok[0] == 0)
    return;
  int64_t now_us = esp_timer_get_time();
  // 1. Check if token already exists, update expiry
  for (size_t i = 0; i < MAX_ADMIN_SESSIONS; ++i) {
    if (s_admin_sessions[i].valid &&
        constant_time_eq(tok, s_admin_sessions[i].token)) {
      s_admin_sessions[i].expiry_us = expiry_us;
      return;
    }
  }
  // 2. Find an empty/invalid or expired slot
  for (size_t i = 0; i < MAX_ADMIN_SESSIONS; ++i) {
    if (!s_admin_sessions[i].valid || now_us > s_admin_sessions[i].expiry_us) {
      snprintf(s_admin_sessions[i].token, sizeof(s_admin_sessions[i].token),
               "%s", tok);
      s_admin_sessions[i].expiry_us = expiry_us;
      s_admin_sessions[i].valid = true;
      return;
    }
  }
  // 3. Overwrite earliest expiring slot if all full
  size_t earliest_idx = 0;
  int64_t earliest_expiry = s_admin_sessions[0].expiry_us;
  for (size_t i = 1; i < MAX_ADMIN_SESSIONS; ++i) {
    if (s_admin_sessions[i].expiry_us < earliest_expiry) {
      earliest_expiry = s_admin_sessions[i].expiry_us;
      earliest_idx = i;
    }
  }
  snprintf(s_admin_sessions[earliest_idx].token,
           sizeof(s_admin_sessions[earliest_idx].token), "%s", tok);
  s_admin_sessions[earliest_idx].expiry_us = expiry_us;
  s_admin_sessions[earliest_idx].valid = true;
}

static void remove_admin_session(const char *tok) {
  if (!tok || tok[0] == 0)
    return;
  for (size_t i = 0; i < MAX_ADMIN_SESSIONS; ++i) {
    if (s_admin_sessions[i].valid &&
        constant_time_eq(tok, s_admin_sessions[i].token)) {
      s_admin_sessions[i].valid = false;
      s_admin_sessions[i].token[0] = 0;
      return;
    }
  }
}

static bool is_admin_authenticated(httpd_req_t *req) {
  // 1. Check Cookie header (up to 512 bytes for complex browser headers)
  char cookie_hdr[512];
  size_t buf_len = httpd_req_get_hdr_value_len(req, "Cookie");
  if (buf_len > 0 && buf_len < sizeof(cookie_hdr)) {
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr,
                                    sizeof(cookie_hdr)) == ESP_OK) {
      char cookie_val[68] = {0};
      char *p = strstr(cookie_hdr, "session_id=");
      if (p) {
        p += strlen("session_id=");
        char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= sizeof(cookie_val))
          n = sizeof(cookie_val) - 1;
        memcpy(cookie_val, p, n);
        cookie_val[n] = 0;
        if (is_token_valid(cookie_val))
          return true;
      }
    }
  }

  // 2. Also check custom header X-Session-Token (prevents cookie loss /
  // SameSite issues)
  char token_hdr[68] = {0};
  size_t tok_len = httpd_req_get_hdr_value_len(req, "X-Session-Token");
  if (tok_len > 0 && tok_len < sizeof(token_hdr)) {
    if (httpd_req_get_hdr_value_str(req, "X-Session-Token", token_hdr,
                                    sizeof(token_hdr)) == ESP_OK) {
      if (is_token_valid(token_hdr))
        return true;
    }
  }

  return false;
}

static bool require_admin(httpd_req_t *req) {
  if (!is_admin_authenticated(req)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(
        req,
        "{\"ok\":false,\"error\":\"unauthorized\",\"auth_required\":true}");
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// ZERO-RAM GZIP WEB UI SERVING
// Serves pre-compressed HTML frontend directly from Flash .rodata with
// Content-Encoding: gzip
// -----------------------------------------------------------------------------
static esp_err_t root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control",
                     "no-cache, no-store, must-revalidate");

  const char *ptr = (const char *)PAGE_GZ;
  size_t remaining = PAGE_GZ_LEN;
  while (remaining > 0) {
    size_t chunk_size = (remaining > 4096) ? 4096 : remaining;
    esp_err_t err = httpd_resp_send_chunk(req, ptr, chunk_size);
    if (err != ESP_OK)
      return err;
    ptr += chunk_size;
    remaining -= chunk_size;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

// -----------------------------------------------------------------------------
// CHUNKED JSON STREAMING: /api/devices
// Uses static stack buffer (0 bytes heap allocated)
// -----------------------------------------------------------------------------
static esp_err_t devices_handler(httpd_req_t *req) {
  wifi_manager_refresh_clients();

  active_client_t active[MAX_CLIENTS];
  size_t count;
  uint16_t period_days;
  uint32_t elapsed_s;
  uint64_t total_up = 0, total_down = 0;
  uint32_t live_total_up = 0, live_total_down = 0;
  uint64_t m_quota = 0;
  char q_start[12] = {0}, q_end[12] = {0};

  portENTER_CRITICAL(&data_mux);
  memcpy(active, active_clients, sizeof(active));
  count = usage_count;
  if (count > MAX_RECORDS)
    count = MAX_RECORDS;
  period_days = meter_period_days;
  elapsed_s = meter_period_elapsed_s;
  live_total_up = total_cur_up_bps;
  live_total_down = total_cur_down_bps;
  m_quota = main_quota_bytes;
  snprintf(q_start, sizeof(q_start), "%s", quota_start_date);
  snprintf(q_end, sizeof(q_end), "%s", quota_end_date);
  for (size_t i = 0; i < count; ++i) {
    uint64_t u_up = usage_records[i].upload_bytes;
    uint64_t u_down = usage_records[i].download_bytes;
    if (router_meter_scale_mode == 0 && router_meter_scale_pct != 100) {
      u_up = (u_up * (uint64_t)router_meter_scale_pct) / 100ULL;
      u_down = (u_down * (uint64_t)router_meter_scale_pct) / 100ULL;
    }
    total_up += u_up;
    total_down += u_down;
  }
  portEXIT_CRITICAL(&data_mux);

  wifi_ap_record_t upstream_record;
  bool wan_connected = (esp_wifi_sta_get_ap_info(&upstream_record) == ESP_OK);
  uint32_t free_heap = esp_get_free_heap_size();
  uint32_t min_free_heap = esp_get_minimum_free_heap_size();
  uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  int8_t sta_rssi = wan_connected ? upstream_record.rssi : 0;
  int sta_channel = wan_connected ? upstream_record.primary : 0;
  char sta_ssid_safe[33] = {0};
  if (wan_connected) {
    size_t slen = strlen((char *)upstream_record.ssid);
    if (slen > 32)
      slen = 32;
    memcpy(sta_ssid_safe, upstream_record.ssid, slen);
  }
  uint16_t active_clients_count = 0;
  for (size_t j = 0; j < MAX_CLIENTS; ++j) {
    if (valid_device_mac(active[j].mac) && active[j].aid != 0) {
      active_clients_count++;
    }
  }

  esp_netif_ip_info_t sta_ip_info = {0};
  char current_sta_ip[16] = {0};
  char current_sta_gw[16] = {0};
  char current_sta_mask[16] = {0};
  if (esp_netif_get_ip_info(sta_netif, &sta_ip_info) == ESP_OK &&
      sta_ip_info.ip.addr != 0) {
    snprintf(current_sta_ip, sizeof(current_sta_ip), IPSTR,
             IP2STR(&sta_ip_info.ip));
    snprintf(current_sta_gw, sizeof(current_sta_gw), IPSTR,
             IP2STR(&sta_ip_info.gw));
    snprintf(current_sta_mask, sizeof(current_sta_mask), IPSTR,
             IP2STR(&sta_ip_info.netmask));
  } else if (sta_static_enabled) {
    snprintf(current_sta_ip, sizeof(current_sta_ip), "%s", sta_ip_text);
    snprintf(current_sta_gw, sizeof(current_sta_gw), "%s", sta_gateway_text);
    snprintf(current_sta_mask, sizeof(current_sta_mask), "%s", sta_mask_text);
  }

  time_t now = time(NULL);
  bool time_synced = (now > 1700000000);
  int32_t rem_days = -1;
  int32_t rem_hours = -1;
  if (time_synced && q_end[0]) {
    time_t end_ts = meter_parse_date(q_end, true);
    if (end_ts > now) {
      int64_t diff = (int64_t)(end_ts - now);
      rem_days = (int32_t)(diff / 86400);
      rem_hours = (int32_t)((diff % 86400) / 3600);
    } else {
      rem_days = 0;
      rem_hours = 0;
    }
  }
  double gb_div =
      (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  double main_quota_gb = (double)m_quota / gb_div;
  uint64_t grand_total_bytes = total_up + total_down;
  double grand_total_gb = (double)grand_total_bytes / gb_div;
  double rem_main_gb = 0.0;
  if (main_quota_gb > 0.0) {
    rem_main_gb =
        main_quota_gb > grand_total_gb ? (main_quota_gb - grand_total_gb) : 0.0;
  }

  bool is_admin = is_admin_authenticated(req);

  ota_info_t ota_inf;
  ota_updater_get_info(&ota_inf);

  char router_time_str[16] = "--:--";
  time_t cur_epoch = time(NULL);
  if (cur_epoch > 1700000000) {
    time_t local_cur = cur_epoch + ((int64_t)auto_reboot_tz_offset * 3600LL);
    struct tm cur_tm;
    gmtime_r(&local_cur, &cur_tm);
    snprintf(router_time_str, sizeof(router_time_str), "%02d:%02d",
             cur_tm.tm_hour, cur_tm.tm_min);
  }
  char reboot_time_str[8];
  snprintf(reboot_time_str, sizeof(reboot_time_str), "%02d:%02d",
           auto_reboot_hour, auto_reboot_min);

  // Stack buffer: 2048 bytes ensures header string (approx ~1000 bytes) will
  // never truncate or overflow
  char buf[2048];
  httpd_resp_set_type(req, "application/json; charset=utf-8");

  size_t used = (size_t)snprintf(
      buf, sizeof(buf),
      "{\"wan\":%s,\"is_admin\":%s,\"period_days\":%u,\"elapsed_s\":%" PRIu32
      ","
      "\"total_up\":%" PRIu64 ",\"total_down\":%" PRIu64 ","
      "\"cur_up_bps\":%" PRIu32 ",\"cur_down_bps\":%" PRIu32 ","
      "\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32
      ",\"uptime_s\":%" PRIu32 ","
      "\"sta_rssi\":%d,\"sta_channel\":%d,\"active_clients\":%u,"
      "\"cpu_freq_mhz\":%u,\"wifi_power_save\":%s,\"meter_scale_pct\":%u,"
      "\"meter_scale_mode\":%u,\"gb_"
      "standard\":%u,"
      "\"main_quota_gb\":%.2f,\"q_start\":\"%s\",\"q_end\":\"%s\","
      "\"time_synced\":%s,\"rem_days\":%ld,\"rem_hours\":%ld,\"rem_main_gb\":%."
      "3f,"
      "\"firmware_version\":\"" ROUTER_FIRMWARE_VERSION
      "\",\"running_partition\":\"%s\",\"sta_ssid\":",
      wan_connected ? "true" : "false", is_admin ? "true" : "false",
      period_days, elapsed_s, total_up, total_down, live_total_up,
      live_total_down, free_heap, min_free_heap, uptime_s, sta_rssi,
      sta_channel, active_clients_count, (unsigned)esp_cpu_freq_mhz,
      esp_wifi_power_save ? "true" : "false", (unsigned)router_meter_scale_pct,
      (unsigned)router_meter_scale_mode, (unsigned)router_gb_standard,
      main_quota_gb, q_start, q_end, time_synced ? "true" : "false",
      (long)rem_days, (long)rem_hours, rem_main_gb, ota_inf.running_partition);
  if (used >= sizeof(buf) - 64)
    used = sizeof(buf) - 64;
  used = append_json_string(buf, sizeof(buf), used, sta_ssid_safe);
  if (used < sizeof(buf) - 1) {
    used += (size_t)snprintf(
        buf + used, sizeof(buf) - used,
        ",\"sta_static_enabled\":%s,\"sta_ip\":\"%s\",\"sta_mask\":\"%s\","
        "\"sta_gw\":\"%s\",\"sta_dns\":\"%s\","
        "\"wifi_proto\":%u,\"wifi_band\":%u,\"current_band\":\"%s\",\"router_"
        "ssid\":",
        sta_static_enabled ? "true" : "false", current_sta_ip, current_sta_mask,
        current_sta_gw, sta_dns_text, (unsigned)wifi_protocol_mode,
        (unsigned)wifi_band_mode, wifi_manager_get_current_band_str());
  }
  if (used >= sizeof(buf) - 64)
    used = sizeof(buf) - 64;
  used = append_json_string(buf, sizeof(buf), used, router_ssid);
  if (used < sizeof(buf) - 16) {
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, ",\"ap_ssid\":");
  }
  if (used >= sizeof(buf) - 64)
    used = sizeof(buf) - 64;
  used = append_json_string(buf, sizeof(buf), used, ap_ssid);
  if (used < sizeof(buf) - 1) {
    used += (size_t)snprintf(
        buf + used, sizeof(buf) - used,
        ",\"tg_enable\":%s,\"tg_interactive\":%s,\"tg_chat_id\":\"%s\",\"tg_"
        "thresholds\":\"%s\",\"rb_enable\":%s,\"rb_time\":\"%s\",\"rb_tz\":%d,"
        "\"router_clock\":\"%s\","
        "\"ota_auto\":%s,\"last_reset_reason\":\"%s\",\"last_reset_code\":%d",
        telegram_enabled ? "true" : "false",
        telegram_interactive ? "true" : "false", telegram_chat_id,
        telegram_thresholds, auto_reboot_enabled ? "true" : "false",
        reboot_time_str, auto_reboot_tz_offset, router_time_str,
        ota_auto_update_enabled ? "true" : "false", last_reset_reason_str,
        last_reset_code);
  }

  // Output discovered Telegram users list for dropdown linking
  tg_discovered_user_t disc_users[MAX_DISCOVERED_TG_USERS];
  size_t disc_cnt =
      telegram_get_discovered_users(disc_users, MAX_DISCOVERED_TG_USERS);
  if (used < sizeof(buf) - 32) {
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, ",\"tg_users\":[");
    for (size_t u = 0; u < disc_cnt; ++u) {
      if (used < sizeof(buf) - 128) {
        used += (size_t)snprintf(
            buf + used, sizeof(buf) - used,
            "%s{\"chat_id\":\"%s\",\"name\":\"%s\",\"username\":\"%s\"}",
            (u == 0) ? "" : ",", disc_users[u].chat_id,
            disc_users[u].first_name, disc_users[u].username);
      }
    }
    if (used < sizeof(buf) - 2) {
      buf[used++] = ']';
      buf[used] = 0;
    }
  }

  if (used < sizeof(buf) - 16) {
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, ",\"devices\":[");
  }

  esp_err_t err = httpd_resp_send_chunk(req, buf, used);
  if (err != ESP_OK)
    return err;

  bool first = true;
  uint8_t seen_macs[MAX_RECORDS + MAX_CLIENTS][6];
  size_t seen_count = 0;
  used = 0;

  for (size_t i = 0; i < count; ++i) {
    usage_record_t rec;
    device_policy_t pol = {0};
    char name[DEVICE_NAME_BYTES] = {0};
    device_speed_t spd = {0};
    device_live_speed_t live = {0};
    bool valid = false;

    portENTER_CRITICAL(&data_mux);
    if (i < usage_count && valid_device_mac(usage_records[i].mac)) {
      if (usage_records[i].mac[0] == 0x02 && usage_records[i].mac[1] == 0x00) {
        portEXIT_CRITICAL(&data_mux);
        continue;
      }
      if (usage_records[i].mac[0] == 0x02 && usage_records[i].mac[1] == 0xE0) {
        uint32_t rip = 0;
        memcpy(&rip, usage_records[i].mac + 2, 4);
        bool claimed_by_real = false;
        for (size_t b = 0; b < binding_count; ++b) {
          if (device_bindings[b].bound_ip == rip &&
              valid_device_mac(device_bindings[b].mac) &&
              !(device_bindings[b].mac[0] == 0x02 &&
                (device_bindings[b].mac[1] == 0x00 ||
                 device_bindings[b].mac[1] == 0xE0))) {
            claimed_by_real = true;
            break;
          }
        }
        if (claimed_by_real) {
          portEXIT_CRITICAL(&data_mux);
          continue;
        }
      }
      valid = true;
      rec = usage_records[i];
      live = device_live_speeds[i];
      int p_idx = meter_find_policy_locked(rec.mac, false);
      if (p_idx >= 0)
        pol = device_policies[p_idx];
      int n_idx = meter_find_name_locked(rec.mac, false);
      if (n_idx >= 0) {
        snprintf(name, sizeof(name), "%s", device_names[n_idx].name);
      }
      int s_idx = meter_find_speed_locked(rec.mac, false);
      if (s_idx >= 0)
        spd = device_speeds[s_idx];
    }
    portEXIT_CRITICAL(&data_mux);

    if (!valid)
      continue;

    bool already_seen = false;
    for (size_t s = 0; s < seen_count; ++s) {
      if (memcmp(seen_macs[s], rec.mac, 6) == 0) {
        already_seen = true;
        break;
      }
    }
    if (already_seen)
      continue;
    if (seen_count < sizeof(seen_macs) / sizeof(seen_macs[0])) {
      memcpy(seen_macs[seen_count++], rec.mac, 6);
    }

    int active_index = -1;
    for (size_t j = 0; j < MAX_CLIENTS; ++j) {
      if (memcmp(active[j].mac, rec.mac, 6) == 0) {
        active_index = (int)j;
        break;
      }
    }

    char mac[18];
    format_mac(rec.mac, mac);
    char canon_mac[18];
    format_mac(rec.mac, canon_mac);

    portENTER_CRITICAL(&data_mux);
    int b_idx = meter_find_binding_locked(rec.mac, false);
    uint32_t bound_ip = (b_idx >= 0) ? device_bindings[b_idx].bound_ip : 0;
    portEXIT_CRITICAL(&data_mux);

    char ip_text[16] = "";
    if (active_index >= 0 && active[active_index].ip != 0) {
      ip4_addr_t bip = {.addr = active[active_index].ip};
      snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&bip));
    } else if (bound_ip != 0) {
      ip4_addr_t bip = {.addr = bound_ip};
      snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&bip));
    } else if (rec.mac[0] == 0x02 && rec.mac[1] == 0xE0) {
      snprintf(ip_text, sizeof(ip_text), "%u.%u.%u.%u", (unsigned)rec.mac[2],
               (unsigned)rec.mac[3], (unsigned)rec.mac[4],
               (unsigned)rec.mac[5]);
    }

    int8_t rssi = (active_index >= 0) ? active[active_index].rssi : 0;
    bool connected = (active_index >= 0 && active[active_index].aid != 0 &&
                      active[active_index].ip != 0);
    uint64_t quota = pol.quota_bytes;
    uint32_t upload_limit_bps = spd.upload_bps;
    uint32_t download_limit_bps = spd.download_bps;
    uint64_t rec_up = rec.upload_bytes;
    uint64_t rec_down = rec.download_bytes;
    if (router_meter_scale_mode == 0 && router_meter_scale_pct != 100) {
      rec_up = (rec_up * (uint64_t)router_meter_scale_pct) / 100ULL;
      rec_down = (rec_down * (uint64_t)router_meter_scale_pct) / 100ULL;
    }
    uint64_t dev_tot = rec_up + rec_down;
    bool quota_exceeded = (quota > 0 && dev_tot >= quota);
    bool blocked = pol.blocked || quota_exceeded;
    bool gaming = pol.gaming_mode;
    double dev_share_pct =
        (m_quota > 0) ? (((double)dev_tot / (double)m_quota) * 100.0) : 0.0;
    double dev_quota_pct =
        (quota > 0) ? (((double)dev_tot / (double)quota) * 100.0) : 0.0;
    double gb_div = (router_gb_standard == 1000) ? 1000000000.0
                                                 : (1024.0 * 1024.0 * 1024.0);
    double dev_quota_gb = (double)quota / gb_div;

    // Flush batch if buffer is getting full
    if (used + 450 >= sizeof(buf)) {
      err = httpd_resp_send_chunk(req, buf, used);
      if (err != ESP_OK)
        return err;
      used = 0;
    }

    if (!first) {
      buf[used++] = ',';
    }
    first = false;

    used += (size_t)snprintf(
        buf + used, sizeof(buf) - used,
        "{\"ip\":\"%s\",\"mac\":\"%s\",\"canon_mac\":\"%s\",\"rssi\":%d,\"up\":"
        "%" PRIu64 ",\"down\":%" PRIu64 ","
        "\"quota\":%" PRIu64
        ",\"quota_gb\":%.2f,\"quota_pct\":%.1f,\"quota_exceeded\":%s,"
        "\"upload_mbps\":%.2f,\"download_mbps\":%.2f,"
        "\"cur_up_bps\":%" PRIu32 ",\"cur_down_bps\":%" PRIu32
        ",\"share_pct\":%.2f,\"name\":",
        ip_text, mac, canon_mac, rssi, rec_up, rec_down, quota, dev_quota_gb,
        dev_quota_pct, quota_exceeded ? "true" : "false",
        upload_limit_bps / 1000000.0, download_limit_bps / 1000000.0,
        live.cur_up_bps, live.cur_down_bps, dev_share_pct);

    used = append_json_string(buf, sizeof(buf), used, name);
    used += (size_t)snprintf(
        buf + used, sizeof(buf) - used,
        ",\"connected\":%s,\"blocked\":%s,\"gaming\":%s,\"telegram_chat_id\":",
        connected ? "true" : "false", blocked ? "true" : "false",
        gaming ? "true" : "false");
    used = append_json_string(buf, sizeof(buf), used, pol.telegram_chat_id);

    char bound_ip_str[16] = "";
    if (bound_ip != 0) {
      ip4_addr_t bip = {.addr = bound_ip};
      snprintf(bound_ip_str, sizeof(bound_ip_str), "%d.%d.%d.%d", IP2STR(&bip));
    }

    uint32_t conn_s = 0;
    uint32_t last_seen_ago_s = 0;
    if (connected) {
      if (active_index >= 0 && active[active_index].connected_at_s > 0 && uptime_s >= active[active_index].connected_at_s) {
        conn_s = uptime_s - active[active_index].connected_at_s;
      } else if (device_connected_at_s[i] > 0 && uptime_s >= device_connected_at_s[i]) {
        conn_s = uptime_s - device_connected_at_s[i];
      }
    } else {
      if (device_last_seen_s[i] > 0 && uptime_s >= device_last_seen_s[i]) {
        last_seen_ago_s = uptime_s - device_last_seen_s[i];
      }
    }

    used += (size_t)snprintf(buf + used, sizeof(buf) - used,
                             ",\"is_static_ip\":%s,\"bound_ip\":\"%s\",\"conn_s\":%lu,\"last_seen_ago_s\":%lu",
                             (bound_ip != 0) ? "true" : "false", bound_ip_str,
                             (unsigned long)conn_s, (unsigned long)last_seen_ago_s);
    if (used < sizeof(buf) - 1)
      buf[used++] = '}';
  }

  if (used > 0) {
    err = httpd_resp_send_chunk(req, buf, used);
    if (err != ESP_OK)
      return err;
  }

  err = httpd_resp_send_chunk(req, "]}", 2);
  if (err != ESP_OK)
    return err;
  return httpd_resp_send_chunk(req, NULL, 0);
}

// -----------------------------------------------------------------------------
// AUTHENTICATION: LOGIN, LOGOUT, PASSWORD
// -----------------------------------------------------------------------------
static esp_err_t login_handler(httpd_req_t *req) {
  int64_t now_us = esp_timer_get_time();
  if (lockout_until_us > now_us) {
    uint32_t rem_sec = (uint32_t)((lockout_until_us - now_us) / 1000000ULL);
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char res[90];
    snprintf(res, sizeof(res),
             "{\"ok\":false,\"error\":\"locked_out\",\"remaining\":%lu}",
             (unsigned long)rem_sec);
    return httpd_resp_sendstr(req, res);
  }

  char body[160];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char user[48] = {0};
  char pass[80] = {0};
  form_value(body, "username", user, sizeof(user));
  form_value(body, "password", pass, sizeof(pass));

  bool ok = constant_time_eq(user, admin_username) &&
            constant_time_eq(pass, admin_password);

  if (ok) {
    failed_login_attempts = 0;
    lockout_until_us = 0;
    uint32_t r1 = esp_random(), r2 = esp_random();
    char new_token[65];
    snprintf(new_token, sizeof(new_token), "%08lx%08lx%08lx%08lx",
             (unsigned long)r1, (unsigned long)r2, (unsigned long)esp_random(),
             (unsigned long)esp_random());
    add_admin_session(new_token, now_us + SESSION_TTL_US);

    char set_cookie[160];
    snprintf(set_cookie, sizeof(set_cookie),
             "session_id=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=2592000",
             new_token);
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char res[128];
    snprintf(res, sizeof(res), "{\"ok\":true,\"token\":\"%s\"}", new_token);
    return httpd_resp_sendstr(req, res);
  } else {
    failed_login_attempts++;
    if (failed_login_attempts >= MAX_FAILED_ATTEMPTS) {
      lockout_until_us = now_us + LOCKOUT_DURATION_US;
      httpd_resp_set_status(req, "429 Too Many Requests");
      httpd_resp_set_type(req, "application/json; charset=utf-8");
      return httpd_resp_sendstr(
          req, "{\"ok\":false,\"error\":\"locked_out\",\"remaining\":60}");
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char res[90];
    snprintf(
        res, sizeof(res),
        "{\"ok\":false,\"error\":\"invalid_credentials\",\"attempts_left\":%u}",
        (unsigned)(MAX_FAILED_ATTEMPTS - failed_login_attempts));
    return httpd_resp_sendstr(req, res);
  }
}

static esp_err_t logout_handler(httpd_req_t *req) {
  char token_hdr[68] = {0};
  size_t tok_len = httpd_req_get_hdr_value_len(req, "X-Session-Token");
  if (tok_len > 0 && tok_len < sizeof(token_hdr)) {
    if (httpd_req_get_hdr_value_str(req, "X-Session-Token", token_hdr,
                                    sizeof(token_hdr)) == ESP_OK) {
      remove_admin_session(token_hdr);
    }
  }
  char cookie_hdr[512];
  size_t buf_len = httpd_req_get_hdr_value_len(req, "Cookie");
  if (buf_len > 0 && buf_len < sizeof(cookie_hdr)) {
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr,
                                    sizeof(cookie_hdr)) == ESP_OK) {
      char *p = strstr(cookie_hdr, "session_id=");
      if (p) {
        p += strlen("session_id=");
        char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n < sizeof(token_hdr)) {
          char c_val[68] = {0};
          memcpy(c_val, p, n);
          c_val[n] = 0;
          remove_admin_session(c_val);
        }
      }
    }
  }
  httpd_resp_set_hdr(req, "Set-Cookie", "session_id=; Path=/; Max-Age=0");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t password_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[256];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char old_p[80] = {0}, new_u[48] = {0}, new_p[80] = {0};
  form_value(body, "old_pass", old_p, sizeof(old_p));
  form_value(body, "new_user", new_u, sizeof(new_u));
  form_value(body, "new_pass", new_p, sizeof(new_p));

  if (!constant_time_eq(old_p, admin_password)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"wrong_password\"}");
  }

  if (strlen(new_u) < 2 || strlen(new_p) < 4) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"too_short\"}");
  }

  config_save_admin(new_u, new_p);
  memset(s_admin_sessions, 0, sizeof(s_admin_sessions));
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// DEVICE MANAGEMENT API
// -----------------------------------------------------------------------------
static esp_err_t device_save_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[512];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char mac_text[64] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }

  char name[DEVICE_NAME_BYTES] = {0};
  form_value(body, "name", name, sizeof(name));
  if (name[0]) {
    meter_set_device_name(mac, name);
  }

  char tg_chat[32] = {0};
  form_value(body, "telegram_chat_id", tg_chat, sizeof(tg_chat));
  meter_set_device_telegram(mac, tg_chat);

  char bound_ip_s[24] = {0};
  form_value(body, "bound_ip", bound_ip_s, sizeof(bound_ip_s));
  char is_static_s[8] = {0};
  form_value(body, "is_static_ip", is_static_s, sizeof(is_static_s));

  if (is_static_s[0]) {
    bool is_static =
        (strcmp(is_static_s, "true") == 0 || strcmp(is_static_s, "1") == 0);
    if (!is_static) {
      meter_set_device_binding(mac, 0);
    } else if (bound_ip_s[0]) {
      ip4_addr_t b_ip = {0};
      if (ip4addr_aton(bound_ip_s, &b_ip)) {
        meter_set_device_binding(mac, b_ip.addr);
      }
    }
  } else if (bound_ip_s[0]) {
    if (strcmp(bound_ip_s, "0") == 0 || strcmp(bound_ip_s, "none") == 0) {
      meter_set_device_binding(mac, 0);
    } else {
      ip4_addr_t b_ip = {0};
      if (ip4addr_aton(bound_ip_s, &b_ip)) {
        meter_set_device_binding(mac, b_ip.addr);
      }
    }
  }

  char up_s[20] = {0}, down_s[20] = {0};
  form_value(body, "upload_mbps", up_s, sizeof(up_s));
  form_value(body, "download_mbps", down_s, sizeof(down_s));
  if (up_s[0] || down_s[0]) {
    double up_m = atof(up_s);
    double down_m = atof(down_s);
    uint32_t up_bps = (uint32_t)(up_m * 1000000.0);
    uint32_t down_bps = (uint32_t)(down_m * 1000000.0);
    meter_set_device_speed(mac, up_bps, down_bps);
  }

  char quota_s[24] = {0};
  form_value(body, "quota_gb", quota_s, sizeof(quota_s));
  if (quota_s[0]) {
    double quota_gb = atof(quota_s);
    double gb_div = (router_gb_standard == 1000) ? 1000000000.0
                                                 : (1024.0 * 1024.0 * 1024.0);
    uint64_t quota_bytes = (uint64_t)(quota_gb * gb_div);
    int p_idx = meter_find_policy_locked(mac, true);
    bool blk = (p_idx >= 0) ? device_policies[p_idx].blocked : false;
    bool gam = (p_idx >= 0) ? device_policies[p_idx].gaming_mode : false;

    // Auto-unblock device if newly assigned quota > current usage or set to 0
    // (unlimited)
    int u_idx = meter_find_usage_locked(mac, false);
    if (u_idx >= 0) {
      uint64_t consumed = usage_records[u_idx].upload_bytes +
                          usage_records[u_idx].download_bytes;
      if (quota_bytes == 0 || consumed < quota_bytes) {
        blk = false;
      }
    }
    meter_set_device_policy(mac, quota_bytes, blk, gam);
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_block_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[256];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char mac_text[64] = {0}, val_s[8] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  form_value(body, "blocked", val_s, sizeof(val_s));

  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_set_device_blocked(mac, atoi(val_s) != 0);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_gaming_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[256];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char mac_text[64] = {0}, val_s[8] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  form_value(body, "gaming", val_s, sizeof(val_s));

  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_set_device_gaming(mac, atoi(val_s) != 0);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_reset_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[256];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char mac_text[64] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_reset_device_usage(mac);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_delete_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[256];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char mac_text[64] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_delete_device(mac);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// MAIN ISP QUOTA & PERIOD
// -----------------------------------------------------------------------------
static esp_err_t main_quota_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[200];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char q_gb_s[20] = {0}, s_date[16] = {0}, e_date[16] = {0};
  form_value(body, "quota_gb", q_gb_s, sizeof(q_gb_s));
  form_value(body, "start_date", s_date, sizeof(s_date));
  form_value(body, "end_date", e_date, sizeof(e_date));

  double q_gb = atof(q_gb_s);
  double gb_div =
      (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  uint64_t q_bytes = (uint64_t)(q_gb * gb_div);
  meter_set_main_quota(q_bytes, s_date[0] ? s_date : NULL,
                       e_date[0] ? e_date : NULL);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t meter_period_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[64];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char days_s[12] = {0};
  form_value(body, "period_days", days_s, sizeof(days_s));
  int d = atoi(days_s);
  if (d >= 1 && d <= 365) {
    meter_set_period_days((uint16_t)d);
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t meter_reset_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  meter_reset_all_usage(); // Also clears Telegram alert flags!
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// PERFORMANCE & METER SCALE
// -----------------------------------------------------------------------------
static esp_err_t performance_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[96];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char freq_s[12] = {0}, ps_s[8] = {0};
  form_value(body, "cpu_freq", freq_s, sizeof(freq_s));
  form_value(body, "wifi_ps", ps_s, sizeof(ps_s));

  int f = atoi(freq_s);
  if (f == 80 || f == 160 || f == 240) {
    bool ps = (atoi(ps_s) != 0);
    config_apply_performance((uint16_t)f, ps);
    nvs_handle_t nvs;
    if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_set_u16(nvs, "cpu_mhz", (uint16_t)f);
      nvs_set_u8(nvs, "wifi_ps", ps ? 1 : 0);
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t meter_scale_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[96];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char scale_s[12] = {0}, gb_std_s[12] = {0}, mode_s[12] = {0};
  form_value(body, "scale", scale_s, sizeof(scale_s));
  form_value(body, "gb_std", gb_std_s, sizeof(gb_std_s));
  form_value(body, "mode", mode_s, sizeof(mode_s));
  int sc = atoi(scale_s);
  int gb_std = atoi(gb_std_s);
  int mode = atoi(mode_s);
  if (sc >= 10 && sc <= 200) {
    if (gb_std != 1000 && gb_std != 1024)
      gb_std = 1024;
    if (mode != 0 && mode != 1)
      mode = 0;
    config_save_meter_scale((uint8_t)sc, (uint16_t)gb_std, (uint8_t)mode);
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// SAVE WI-FI SETTINGS & REBOOT
// -----------------------------------------------------------------------------
static esp_err_t save_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[512];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char r_ssid[33] = {0}, r_pass[65] = {0}, clr_r_pass[8] = {0};
  char a_ssid[33] = {0}, a_pass[65] = {0}, proto_s[8] = {0}, band_s[8] = {0};
  char static_s[8] = {0}, ip[16] = {0}, mask[16] = {0}, gw[16] = {0},
       dns[16] = {0};

  form_value(body, "router_ssid", r_ssid, sizeof(r_ssid));
  form_value(body, "router_password", r_pass, sizeof(r_pass));
  form_value(body, "clear_router_password", clr_r_pass, sizeof(clr_r_pass));
  form_value(body, "ap_ssid", a_ssid, sizeof(a_ssid));
  form_value(body, "ap_password", a_pass, sizeof(a_pass));
  form_value(body, "wifi_band", band_s, sizeof(band_s));
  form_value(body, "wifi_protocol", proto_s, sizeof(proto_s));
  form_value(body, "static_enabled", static_s, sizeof(static_s));
  form_value(body, "sta_ip", ip, sizeof(ip));
  form_value(body, "sta_mask", mask, sizeof(mask));
  form_value(body, "sta_gateway", gw, sizeof(gw));
  form_value(body, "sta_dns", dns, sizeof(dns));

  uint8_t band = (uint8_t)atoi(band_s);
  if (band < 1 || band > 3)
    band = 3;
  uint8_t proto = (uint8_t)atoi(proto_s);
  bool static_en = (atoi(static_s) != 0);
  bool clear_pass = (atoi(clr_r_pass) != 0);

  config_save_wifi(r_ssid, r_pass, clear_pass, a_ssid, a_pass, band, proto,
                   static_en, ip, mask, gw, dns);

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(
      req, "<!doctype html><html lang='ar' dir='rtl'><body "
           "style='background:#0f172a;color:#fff;font-family:sans-serif;text-"
           "align:center;padding:50px'><h2>تم حفظ الإعدادات بنجاح!</h2><p>جاري "
           "إعادة تشغيل الراوتر... انتظر 15 ثانية ثم اتصل بالشبكة "
           "الجديدة.</p></body></html>");

  vTaskDelay(pdMS_TO_TICKS(1500));
  meter_save_usage();
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[96];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char cd_min_s[12] = {0}, imm_s[8] = {0};
  form_value(body, "countdown_min", cd_min_s, sizeof(cd_min_s));
  form_value(body, "immediate", imm_s, sizeof(imm_s));

  if (atoi(imm_s) != 0) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true,\"action\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    meter_save_usage();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
  }

  int cd = atoi(cd_min_s);
  if (cd > 0 && cd <= 1440) {
    reboot_countdown_s = cd * 60;
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"countdown_s\":true}");
  }

  httpd_resp_set_status(req, "400 Bad Request");
  return httpd_resp_sendstr(req, "{\"ok\":false}");
}

// -----------------------------------------------------------------------------
// DAILY SCHEDULED AUTO-REBOOT
// -----------------------------------------------------------------------------
static esp_err_t auto_reboot_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[128];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char en_s[8] = {0}, time_s[12] = {0}, tz_s[8] = {0};
  form_value(body, "enable", en_s, sizeof(en_s));
  form_value(body, "time", time_s, sizeof(time_s));
  form_value(body, "tz", tz_s, sizeof(tz_s));

  bool en = (atoi(en_s) != 0);
  int h = 3, m = 0;
  sscanf(time_s, "%d:%d", &h, &m);
  int tz = atoi(tz_s);

  config_save_auto_reboot(en, (uint8_t)h, (uint8_t)m, (int8_t)tz);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// TELEGRAM BOT & MULTI-THRESHOLD QUOTA ALERTS
// -----------------------------------------------------------------------------
static esp_err_t telegram_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;
  char body[512];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char test_s[8] = {0};
  form_value(body, "test", test_s, sizeof(test_s));

  char token[80] = {0}, chat_id[128] = {0}, thresholds[48] = {0};
  form_value(body, "token", token, sizeof(token));
  form_value(body, "chat_id", chat_id, sizeof(chat_id));
  form_value(body, "thresholds", thresholds, sizeof(thresholds));

  if (atoi(test_s) != 0) {
    if (token[0]) {
      snprintf(telegram_token, sizeof(telegram_token), "%s", token);
    }
    if (chat_id[0]) {
      snprintf(telegram_chat_id, sizeof(telegram_chat_id), "%s", chat_id);
    }
    char diag[256] = {0};
    int rc = telegram_send_test_alert_diag(diag, sizeof(diag));
    bool ok = (rc == 0);

    char resp[384];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"msg\":\"%s\"}",
             ok ? "true" : "false", diag);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, resp);
  }

  char en_s[8] = {0}, inter_s[8] = {0};
  form_value(body, "enable", en_s, sizeof(en_s));
  form_value(body, "interactive", inter_s, sizeof(inter_s));

  bool en = (atoi(en_s) != 0);
  bool inter = (atoi(inter_s) != 0);

  // Always enable both if token is present
  config_save_telegram(token[0] ? true : en, token[0] ? true : inter,
                       token[0] ? token : NULL, chat_id[0] ? chat_id : NULL,
                       thresholds[0] ? thresholds : NULL);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(
      req, "{\"ok\":true,\"msg\":\"تم حفظ إعدادات بوت التيليجرام بنجاح! 🤖\"}");
}

// -----------------------------------------------------------------------------
// SECURE OVER-THE-AIR (OTA) AUTOMATED UPDATE HANDLERS
// -----------------------------------------------------------------------------
static esp_err_t ota_status_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;

  ota_info_t info;
  ota_updater_get_info(&info);

  char res[340];
  const char *state_str = "idle";
  switch (info.state) {
  case OTA_STATE_IDLE:
    state_str = "idle";
    break;
  case OTA_STATE_PREPARING:
    state_str = "preparing";
    break;
  case OTA_STATE_DOWNLOADING:
    state_str = "downloading";
    break;
  case OTA_STATE_FLASHING:
    state_str = "flashing";
    break;
  case OTA_STATE_SUCCESS:
    state_str = "success";
    break;
  case OTA_STATE_FAILED:
    state_str = "failed";
    break;
  }

  snprintf(
      res, sizeof(res),
      "{\"version\":\"%s\",\"build_timestamp\":\"%s\","
      "\"running_partition\":\"%s\",\"target_partition\":\"%s\","
      "\"ota_auto\":%s,\"state\":\"%s\",\"progress\":%u,\"written\":%" PRIu32
      ",\"total\":%" PRIu32 ",\"error\":\"%s\"}",
      info.current_version, info.build_timestamp, info.running_partition,
      info.target_partition, ota_auto_update_enabled ? "true" : "false",
      state_str, (unsigned)info.progress_pct, info.written_bytes,
      info.total_bytes, info.error_msg);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, res);
}

static esp_err_t ota_update_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;

  // Check if this is a direct binary file upload (.bin)
  // If Content-Type contains octet-stream or content length is substantial
  // without form urlencoded
  char content_type[64] = {0};
  httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                              sizeof(content_type));

  if (strstr(content_type, "application/octet-stream") != NULL ||
      (req->content_len > 1024 &&
       strstr(content_type, "application/x-www-form-urlencoded") == NULL)) {
    return ota_updater_handle_stream(req);
  }

  // Otherwise, handle form with URL for automated remote OTA or auto_update
  // toggle
  char body[280];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;

  char auto_s[8] = {0};
  form_value(body, "auto_update", auto_s, sizeof(auto_s));
  if (auto_s[0] != 0) {
    bool en = (atoi(auto_s) != 0 || strcmp(auto_s, "true") == 0);
    config_save_ota_auto_update(en);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, en ? "{\"ok\":true,\"ota_auto\":true}"
                                      : "{\"ok\":true,\"ota_auto\":false}");
  }

  char url[220] = {0};
  form_value(body, "url", url, sizeof(url));
  if (url[0] == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing URL parameter or binary file");
    return ESP_FAIL;
  }

  esp_err_t err = ota_updater_start_from_url(url);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (err == ESP_OK) {
    return httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"ota_started\"}");
  } else {
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"ota_start_failed\"}");
  }
}

static esp_err_t ota_check_handler(httpd_req_t *req) {
  char ver[24] = {0};
  char url[256] = {0};
  char log[256] = {0};
  bool update_avail = false;

  esp_err_t err = ota_check_remote_version(ver, sizeof(ver), url, sizeof(url),
                                           log, sizeof(log), &update_avail);

  char res[800];
  if (err == ESP_OK) {
    snprintf(
        res, sizeof(res),
        "{\"ok\":true,\"current_version\":\"%s\",\"latest_version\":\"%s\","
        "\"update_available\":%s,\"firmware_url\":\"%s\",\"changelog\":\"%s\"}",
        ROUTER_FIRMWARE_VERSION, ver, update_avail ? "true" : "false", url,
        log);
  } else {
    snprintf(
        res, sizeof(res),
        "{\"ok\":false,\"error\":\"check_failed\",\"current_version\":\"%s\"}",
        ROUTER_FIRMWARE_VERSION);
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, res);
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  if (!require_admin(req))
    return ESP_OK;

  wifi_ap_record_t *records = NULL;
  uint16_t ap_count = 0;
  esp_err_t err = wifi_manager_scan_networks(&records, &ap_count);

  httpd_resp_set_type(req, "application/json; charset=utf-8");

  if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
    if (records)
      wifi_manager_free_scan_records(records);
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"error\":\"%s\"}",
             esp_err_to_name(err));
    return httpd_resp_sendstr(req, err_buf);
  }

  httpd_resp_send_chunk(req, "{\"ok\":true,\"aps\":[", 18);

  bool first = true;
  char buf[256];

  for (uint16_t i = 0; i < ap_count; i++) {
    // Skip empty SSIDs (hidden networks)
    if (records[i].ssid[0] == '\0')
      continue;

    const char *band = (records[i].primary > 14) ? "5GHz" : "2.4GHz";
    bool is_open = (records[i].authmode == WIFI_AUTH_OPEN);

    // Deduplicate: If an earlier record has the SAME SSID and SAME band, skip
    // this one (since records are sorted by RSSI descending, the earlier one is
    // stronger)
    bool duplicate = false;
    for (uint16_t k = 0; k < i; k++) {
      if (records[k].ssid[0] != '\0' &&
          strncmp((char *)records[k].ssid, (char *)records[i].ssid, 32) == 0) {
        const char *k_band = (records[k].primary > 14) ? "5GHz" : "2.4GHz";
        if (strcmp(k_band, band) == 0) {
          duplicate = true;
          break;
        }
      }
    }
    if (duplicate)
      continue;

    // Escape quotes and backslashes in SSID safely
    char safe_ssid[68] = {0};
    size_t s_idx = 0;
    for (size_t c = 0;
         c < sizeof(records[i].ssid) && records[i].ssid[c] != '\0' &&
         s_idx < sizeof(safe_ssid) - 2;
         c++) {
      char ch = (char)records[i].ssid[c];
      if (ch == '"' || ch == '\\') {
        safe_ssid[s_idx++] = '\\';
        safe_ssid[s_idx++] = ch;
      } else if ((unsigned char)ch < 32) {
        safe_ssid[s_idx++] = ' ';
      } else {
        safe_ssid[s_idx++] = ch;
      }
    }
    safe_ssid[s_idx] = '\0';

    int len = snprintf(buf, sizeof(buf),
                       "%s{\"ssid\":\"%s\",\"rssi\":%d,\"band\":\"%s\","
                       "\"channel\":%d,\"open\":%s}",
                       first ? "" : ",", safe_ssid, (int)records[i].rssi, band,
                       (int)records[i].primary, is_open ? "true" : "false");
    first = false;
    httpd_resp_send_chunk(req, buf, len);
  }

  httpd_resp_send_chunk(req, "]}", 2);
  httpd_resp_send_chunk(req, NULL, 0);

  if (records) {
    wifi_manager_free_scan_records(records);
  }

  return ESP_OK;
}

// -----------------------------------------------------------------------------
// SERVER INITIALIZATION & ROUTE REGISTRATION
// -----------------------------------------------------------------------------
void web_server_start(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;
  config.max_open_sockets = 7;
  config.recv_wait_timeout = 8;
  config.send_wait_timeout = 8;
  config.stack_size = 8192; // 8KB stack for ESP-IDF v6.1 HTTPD (saves 2KB RAM)
  config.max_uri_handlers = 28;
  config.core_id = 0; // ESP32-C5 Unicore (Core 0)

  httpd_uri_t uris[] = {
      {.uri = "/", .method = HTTP_GET, .handler = root_handler},
      {.uri = "/api/devices", .method = HTTP_GET, .handler = devices_handler},
      {.uri = "/api/login", .method = HTTP_POST, .handler = login_handler},
      {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler},
      {.uri = "/api/password",
       .method = HTTP_POST,
       .handler = password_handler},
      {.uri = "/api/device/save",
       .method = HTTP_POST,
       .handler = device_save_handler},
      {.uri = "/api/device/block",
       .method = HTTP_POST,
       .handler = device_block_handler},
      {.uri = "/api/device/gaming",
       .method = HTTP_POST,
       .handler = device_gaming_handler},
      {.uri = "/api/device/reset",
       .method = HTTP_POST,
       .handler = device_reset_handler},
      {.uri = "/api/device/delete",
       .method = HTTP_POST,
       .handler = device_delete_handler},
      {.uri = "/api/main_quota",
       .method = HTTP_POST,
       .handler = main_quota_handler},
      {.uri = "/meter/period",
       .method = HTTP_POST,
       .handler = meter_period_handler},
      {.uri = "/meter/reset",
       .method = HTTP_POST,
       .handler = meter_reset_handler},
      {.uri = "/api/performance",
       .method = HTTP_POST,
       .handler = performance_handler},
      {.uri = "/api/meter_scale",
       .method = HTTP_POST,
       .handler = meter_scale_handler},
      {.uri = "/save", .method = HTTP_POST, .handler = save_handler},
      {.uri = "/api/system/reboot",
       .method = HTTP_POST,
       .handler = reboot_handler},
      {.uri = "/api/auto_reboot",
       .method = HTTP_POST,
       .handler = auto_reboot_handler},
      {.uri = "/api/telegram",
       .method = HTTP_POST,
       .handler = telegram_handler},
      {.uri = "/api/ota/update",
       .method = HTTP_POST,
       .handler = ota_update_handler},
      {.uri = "/api/ota/status",
       .method = HTTP_GET,
       .handler = ota_status_handler},
      {.uri = "/api/ota/check",
       .method = HTTP_GET,
       .handler = ota_check_handler},
      {.uri = "/api/wifi/scan",
       .method = HTTP_GET,
       .handler = wifi_scan_handler},
  };

  if (httpd_start(&web_server, &config) == ESP_OK) {
    size_t count = sizeof(uris) / sizeof(uris[0]);
    for (size_t i = 0; i < count; ++i) {
      httpd_register_uri_handler(web_server, &uris[i]);
    }
    ESP_LOGI(ROUTER_TAG, "High-throughput HTTP Web Server running on Core %d",
             config.core_id);
  } else {
    ESP_LOGE(ROUTER_TAG, "Failed to start HTTP Web Server");
  }
}

void web_server_stop(void) {
  if (web_server) {
    httpd_stop(web_server);
    web_server = NULL;
  }
}
