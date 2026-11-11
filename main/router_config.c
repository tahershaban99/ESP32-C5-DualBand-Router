#include "router_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "router_globals.h"
#include "meter.h"
#include "soc/rtc.h"
#include "esp_wifi.h"

bool has_saved_config = false;
bool sta_static_enabled = false;
char router_ssid[33] = {0};
char router_password[65] = {0};
char ap_ssid[33] = {0};
char ap_password[65] = {0};
char sta_ip_text[16] = {0};
char sta_mask_text[16] = {0};
char sta_gateway_text[16] = {0};
char sta_dns_text[16] = {0};
char admin_username[33] = "admin";
char admin_password[65] = "admin";

uint16_t esp_cpu_freq_mhz = 240;
bool esp_wifi_power_save = false;
uint8_t wifi_protocol_mode = 1;
uint8_t wifi_band_mode = 3; // Default 3 = Auto Dual-Band (2.4GHz + 5GHz)
uint8_t router_meter_scale_pct = 100;
uint16_t router_gb_standard = 1024;
uint8_t router_meter_scale_mode = 0; // 0 = Dynamic/Retroactive (Instant), 1 = Cumulative (Live Packets)

// Daily Scheduled Auto-Reboot
bool auto_reboot_enabled = false;
uint8_t auto_reboot_hour = 3;  // Default 03:00 AM
uint8_t auto_reboot_min = 0;
int8_t auto_reboot_tz_offset = 2; // Default UTC+2 (Cairo/Jerusalem)
bool ota_auto_update_enabled = true; // Default enabled: auto check & update from GitHub

// Telegram Settings
bool telegram_enabled = false;
bool telegram_interactive = false;
char telegram_token[80] = {0};
char telegram_chat_id[128] = {0};
char telegram_thresholds[48] = "50,75,90,95,100"; // Default multi-thresholds
uint32_t telegram_alert_flags = 0;

size_t parse_telegram_thresholds(const char *str, uint8_t out_vals[16]) {
  if (!str || !out_vals) return 0;
  size_t count = 0;
  int cur_val = 0;
  bool in_num = false;
  const unsigned char *p = (const unsigned char *)str;

  while (*p) {
    int digit = -1;
    if (*p >= '0' && *p <= '9') {
      digit = *p - '0';
      p++;
    } else if (p[0] == 0xD9 && p[1] >= 0xA0 && p[1] <= 0xA9) {
      // Eastern Arabic numerals (٠..٩)
      digit = p[1] - 0xA0;
      p += 2;
    } else if (p[0] == 0xDB && p[1] >= 0xB0 && p[1] <= 0xB9) {
      // Extended Arabic-Indic numerals (۰..۹)
      digit = p[1] - 0xB0;
      p += 2;
    } else {
      p++;
    }

    if (digit >= 0) {
      cur_val = cur_val * 10 + digit;
      if (cur_val > 1000) cur_val = 1000;
      in_num = true;
    } else if (in_num) {
      if (cur_val >= 1 && cur_val <= 100 && count < 16) {
        uint8_t v = (uint8_t)cur_val;
        bool exists = false;
        for (size_t i = 0; i < count; ++i) {
          if (out_vals[i] == v) {
            exists = true;
            break;
          }
        }
        if (!exists) {
          out_vals[count++] = v;
        }
      }
      cur_val = 0;
      in_num = false;
    }
  }
  if (in_num && cur_val >= 1 && cur_val <= 100 && count < 16) {
    uint8_t v = (uint8_t)cur_val;
    bool exists = false;
    for (size_t i = 0; i < count; ++i) {
      if (out_vals[i] == v) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      out_vals[count++] = v;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      if (out_vals[j] < out_vals[i]) {
        uint8_t tmp = out_vals[i];
        out_vals[i] = out_vals[j];
        out_vals[j] = tmp;
      }
    }
  }
  return count;
}

void config_load(void) {
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READONLY, &nvs) != ESP_OK) return;

  size_t len = sizeof(router_ssid);
  nvs_get_str(nvs, "router_ssid", router_ssid, &len);

  len = sizeof(router_password);
  nvs_get_str(nvs, "router_pass", router_password, &len);

  len = sizeof(ap_ssid);
  nvs_get_str(nvs, "ap_ssid", ap_ssid, &len);

  len = sizeof(ap_password);
  nvs_get_str(nvs, "ap_pass", ap_password, &len);

  uint8_t static_enabled = 0;
  nvs_get_u8(nvs, "sta_static", &static_enabled);
  sta_static_enabled = (static_enabled != 0);

  len = sizeof(sta_ip_text);
  nvs_get_str(nvs, "sta_ip", sta_ip_text, &len);

  len = sizeof(sta_mask_text);
  nvs_get_str(nvs, "sta_mask", sta_mask_text, &len);

  len = sizeof(sta_gateway_text);
  nvs_get_str(nvs, "sta_gw", sta_gateway_text, &len);

  len = sizeof(sta_dns_text);
  nvs_get_str(nvs, "sta_dns", sta_dns_text, &len);

  len = sizeof(admin_username);
  if (nvs_get_str(nvs, "admin_user", admin_username, &len) != ESP_OK || !admin_username[0]) {
    strncpy(admin_username, "admin", sizeof(admin_username) - 1);
  }

  len = sizeof(admin_password);
  if (nvs_get_str(nvs, "admin_pass", admin_password, &len) != ESP_OK || !admin_password[0]) {
    strncpy(admin_password, "admin", sizeof(admin_password) - 1);
  }

  has_saved_config = router_ssid[0] && ap_ssid[0] && strlen(ap_password) >= 8;

  uint16_t loaded_freq = 0;
  if (nvs_get_u16(nvs, "cpu_mhz", &loaded_freq) == ESP_OK &&
      (loaded_freq == 80 || loaded_freq == 160 || loaded_freq == 240)) {
    esp_cpu_freq_mhz = loaded_freq;
  } else {
    esp_cpu_freq_mhz = 240;
  }

  uint8_t loaded_ps = 0;
  if (nvs_get_u8(nvs, "wifi_ps", &loaded_ps) == ESP_OK) {
    esp_wifi_power_save = (loaded_ps != 0);
  } else {
    esp_wifi_power_save = false;
  }

  uint8_t loaded_scale = 100;
  if (nvs_get_u8(nvs, "meter_scale", &loaded_scale) == ESP_OK && loaded_scale >= 10 && loaded_scale <= 200) {
    router_meter_scale_pct = loaded_scale;
  } else {
    router_meter_scale_pct = 100;
  }
  uint16_t loaded_gb_std = 1024;
  if (nvs_get_u16(nvs, "gb_std", &loaded_gb_std) == ESP_OK && (loaded_gb_std == 1000 || loaded_gb_std == 1024)) {
    router_gb_standard = loaded_gb_std;
  } else {
    router_gb_standard = 1024;
  }
  uint8_t loaded_mode = 0;
  if (nvs_get_u8(nvs, "meter_mode", &loaded_mode) == ESP_OK && loaded_mode <= 1) {
    router_meter_scale_mode = loaded_mode;
  } else {
    router_meter_scale_mode = 0;
  }

  uint8_t loaded_proto = 1;
  if (nvs_get_u8(nvs, "wifi_proto", &loaded_proto) == ESP_OK && loaded_proto <= 5) {
    wifi_protocol_mode = loaded_proto;
  }

  uint8_t loaded_band = 3;
  if (nvs_get_u8(nvs, "wifi_band", &loaded_band) == ESP_OK && loaded_band >= 1 && loaded_band <= 3) {
    wifi_band_mode = loaded_band;
  } else {
    wifi_band_mode = 3;
  }

  // Auto Reboot
  uint8_t loaded_rb_en = 0;
  if (nvs_get_u8(nvs, "rb_en", &loaded_rb_en) == ESP_OK) {
    auto_reboot_enabled = (loaded_rb_en != 0);
  }
  uint8_t loaded_rb_h = 3;
  if (nvs_get_u8(nvs, "rb_h", &loaded_rb_h) == ESP_OK && loaded_rb_h < 24) {
    auto_reboot_hour = loaded_rb_h;
  }
  uint8_t loaded_rb_m = 0;
  if (nvs_get_u8(nvs, "rb_m", &loaded_rb_m) == ESP_OK && loaded_rb_m < 60) {
    auto_reboot_min = loaded_rb_m;
  }
  int8_t loaded_rb_tz = 2;
  if (nvs_get_i8(nvs, "rb_tz", &loaded_rb_tz) == ESP_OK && loaded_rb_tz >= -12 && loaded_rb_tz <= 14) {
    auto_reboot_tz_offset = loaded_rb_tz;
  }

  uint8_t loaded_ota_auto = 1;
  if (nvs_get_u8(nvs, "ota_auto", &loaded_ota_auto) == ESP_OK) {
    ota_auto_update_enabled = (loaded_ota_auto != 0);
  } else {
    ota_auto_update_enabled = true;
  }

  // Telegram Settings
  uint8_t loaded_tg_en = 1;
  if (nvs_get_u8(nvs, "tg_en", &loaded_tg_en) == ESP_OK) {
    telegram_enabled = (loaded_tg_en != 0);
  } else {
    telegram_enabled = true;
  }
  uint8_t loaded_tg_inter = 1;
  if (nvs_get_u8(nvs, "tg_inter", &loaded_tg_inter) == ESP_OK) {
    telegram_interactive = (loaded_tg_inter != 0);
  } else {
    telegram_interactive = true;
  }
  len = sizeof(telegram_token);
  nvs_get_str(nvs, "tg_token", telegram_token, &len);

  len = sizeof(telegram_chat_id);
  nvs_get_str(nvs, "tg_chat_id", telegram_chat_id, &len);

  len = sizeof(telegram_thresholds);
  bool need_thresh_upgrade = false;
  if (nvs_get_str(nvs, "tg_thresh", telegram_thresholds, &len) != ESP_OK || !telegram_thresholds[0]) {
    strncpy(telegram_thresholds, "50,75,90,95,100", sizeof(telegram_thresholds) - 1);
  } else if (strcmp(telegram_thresholds, "50,75,90,95") == 0 ||
             strcmp(telegram_thresholds, "50, 75, 90, 95") == 0) {
    strncpy(telegram_thresholds, "50,75,90,95,100", sizeof(telegram_thresholds) - 1);
    need_thresh_upgrade = true;
  }
  nvs_get_u32(nvs, "tg_flags", &telegram_alert_flags);
  uint8_t flg_v2 = 0;
  bool need_flg_v2_save = false;
  if (nvs_get_u8(nvs, "tg_flg_v2", &flg_v2) != ESP_OK || flg_v2 == 0) {
    telegram_alert_flags = 0;
    need_flg_v2_save = true;
  }

  // If token is configured, auto-activate both alerts and interactive bot
  if (telegram_token[0]) {
    telegram_enabled = true;
    telegram_interactive = true;
  }

  nvs_close(nvs);

  if (need_thresh_upgrade || need_flg_v2_save) {
    nvs_handle_t rw_nvs;
    if (nvs_open("router", NVS_READWRITE, &rw_nvs) == ESP_OK) {
      if (need_thresh_upgrade) {
        nvs_set_str(rw_nvs, "tg_thresh", telegram_thresholds);
      }
      if (need_flg_v2_save) {
        nvs_set_u32(rw_nvs, "tg_flags", 0);
        nvs_set_u8(rw_nvs, "tg_flg_v2", 1);
      }
      nvs_commit(rw_nvs);
      nvs_close(rw_nvs);
    }
  }
}

void config_save_admin(const char *user, const char *pass) {
  if (!user || !pass || strlen(user) == 0 || strlen(user) > 32 || strlen(pass) == 0 || strlen(pass) > 64) {
    return;
  }
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) != ESP_OK) return;
  nvs_set_str(nvs, "admin_user", user);
  nvs_set_str(nvs, "admin_pass", pass);
  nvs_commit(nvs);
  nvs_close(nvs);

  portENTER_CRITICAL(&data_mux);
  strncpy(admin_username, user, sizeof(admin_username) - 1);
  admin_username[sizeof(admin_username) - 1] = 0;
  strncpy(admin_password, pass, sizeof(admin_password) - 1);
  admin_password[sizeof(admin_password) - 1] = 0;
  portEXIT_CRITICAL(&data_mux);
}

void config_reset_admin_default(void) {
  config_save_admin("admin", "admin");
}

void config_apply_performance(uint16_t cpu_freq, bool wifi_ps) {
  if (cpu_freq == 80 || cpu_freq == 160 || cpu_freq == 240) {
    rtc_cpu_freq_config_t conf;
    if (rtc_clk_cpu_freq_mhz_to_config(cpu_freq, &conf)) {
      rtc_clk_cpu_freq_set_config_fast(&conf);
      esp_cpu_freq_mhz = cpu_freq;
      ESP_LOGI(ROUTER_TAG, "CPU frequency switched to %u MHz", (unsigned)cpu_freq);
    }
  }
  esp_wifi_set_ps(wifi_ps ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
  esp_wifi_power_save = wifi_ps;
  ESP_LOGI(ROUTER_TAG, "Wi-Fi Power Save mode set to: %s", wifi_ps ? "ENABLED (Min Modem)" : "DISABLED (Full Speed)");
}

void config_save_performance(uint16_t cpu_freq, bool wifi_ps) {
  config_apply_performance(cpu_freq, wifi_ps);
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u16(nvs, "cpu_mhz", esp_cpu_freq_mhz);
    nvs_set_u8(nvs, "wifi_ps", esp_wifi_power_save ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_meter_scale(uint8_t scale_pct, uint16_t gb_std, uint8_t scale_mode) {
  if (scale_pct < 10 || scale_pct > 200) return;
  router_meter_scale_pct = scale_pct;
  if (gb_std == 1000 || gb_std == 1024) {
    router_gb_standard = gb_std;
  }
  router_meter_scale_mode = (scale_mode <= 1) ? scale_mode : 0;
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "meter_scale", scale_pct);
    nvs_set_u16(nvs, "gb_std", router_gb_standard);
    nvs_set_u8(nvs, "meter_mode", router_meter_scale_mode);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_auto_reboot(bool enabled, uint8_t hour, uint8_t min, int8_t tz) {
  auto_reboot_enabled = enabled;
  auto_reboot_hour = (hour < 24) ? hour : 3;
  auto_reboot_min = (min < 60) ? min : 0;
  auto_reboot_tz_offset = (tz >= -12 && tz <= 14) ? tz : 2;

  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "rb_en", auto_reboot_enabled ? 1 : 0);
    nvs_set_u8(nvs, "rb_h", auto_reboot_hour);
    nvs_set_u8(nvs, "rb_m", auto_reboot_min);
    nvs_set_i8(nvs, "rb_tz", auto_reboot_tz_offset);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_telegram(bool enabled, bool interactive, const char *token, const char *chat_id, const char *thresholds) {
  if (token) {
    strncpy(telegram_token, token, sizeof(telegram_token) - 1);
    telegram_token[sizeof(telegram_token) - 1] = 0;
  }
  if (chat_id) {
    strncpy(telegram_chat_id, chat_id, sizeof(telegram_chat_id) - 1);
    telegram_chat_id[sizeof(telegram_chat_id) - 1] = 0;
  }

  bool policies_updated = false;
  if (thresholds && thresholds[0]) {
    uint8_t old_vals[16] = {0};
    size_t old_cnt = parse_telegram_thresholds(telegram_thresholds, old_vals);

    strncpy(telegram_thresholds, thresholds, sizeof(telegram_thresholds) - 1);
    telegram_thresholds[sizeof(telegram_thresholds) - 1] = 0;

    uint8_t new_vals[16] = {0};
    size_t new_cnt = parse_telegram_thresholds(telegram_thresholds, new_vals);

    portENTER_CRITICAL(&data_mux);
    uint64_t total_used = 0;
    for (size_t i = 0; i < usage_count; ++i) {
      total_used += meter_calc_scaled_bytes(usage_records[i].upload_bytes, router_meter_scale_pct, router_meter_scale_mode) +
                    meter_calc_scaled_bytes(usage_records[i].download_bytes, router_meter_scale_pct, router_meter_scale_mode);
    }
    uint32_t main_pct = (main_quota_bytes > 0) ? (uint32_t)((total_used * 100ULL) / main_quota_bytes) : 0;

    uint8_t main_max_alerted = 0;
    for (size_t i = 0; i < old_cnt; ++i) {
      if ((telegram_alert_flags & (1UL << i)) && old_vals[i] <= main_pct) {
        if (old_vals[i] > main_max_alerted) main_max_alerted = old_vals[i];
      }
    }
    uint32_t new_main_flags = 0;
    for (size_t j = 0; j < new_cnt; ++j) {
      if (new_vals[j] <= main_pct) {
        bool was_alerted = false;
        for (size_t i = 0; i < old_cnt; ++i) {
          if (old_vals[i] == new_vals[j] && (telegram_alert_flags & (1UL << i))) {
            was_alerted = true;
            break;
          }
        }
        if (was_alerted || new_vals[j] < main_max_alerted) {
          new_main_flags |= (1UL << j);
        }
      }
    }
    telegram_alert_flags = new_main_flags;

    for (size_t p = 0; p < policy_count; ++p) {
      if (device_policies[p].quota_bytes == 0) continue;
      int u = meter_find_usage_locked(device_policies[p].mac, false);
      uint64_t dev_used = 0;
      if (u >= 0) {
        dev_used = meter_calc_scaled_bytes(usage_records[u].upload_bytes, router_meter_scale_pct, router_meter_scale_mode) +
                   meter_calc_scaled_bytes(usage_records[u].download_bytes, router_meter_scale_pct, router_meter_scale_mode);
      }
      uint32_t dev_pct = (uint32_t)((dev_used * 100ULL) / device_policies[p].quota_bytes);
      uint16_t old_dev_flags = device_policies[p].alert_flags;
      uint8_t dev_max_alerted = 0;
      for (size_t i = 0; i < old_cnt; ++i) {
        if ((old_dev_flags & (1U << i)) && old_vals[i] <= dev_pct) {
          if (old_vals[i] > dev_max_alerted) dev_max_alerted = old_vals[i];
        }
      }
      uint16_t new_dev_flags = 0;
      for (size_t j = 0; j < new_cnt; ++j) {
        if (new_vals[j] <= dev_pct) {
          bool was_alerted = false;
          for (size_t i = 0; i < old_cnt; ++i) {
            if (old_vals[i] == new_vals[j] && (old_dev_flags & (1U << i))) {
              was_alerted = true;
              break;
            }
          }
          if (was_alerted || new_vals[j] < dev_max_alerted) {
            new_dev_flags |= (1U << j);
          }
        }
      }
      if (device_policies[p].alert_flags != new_dev_flags) {
        device_policies[p].alert_flags = new_dev_flags;
        policy_dirty = true;
        policies_updated = true;
      }
    }
    portEXIT_CRITICAL(&data_mux);
  }

  // Force true if token is set
  telegram_enabled = (telegram_token[0] != 0) ? true : enabled;
  telegram_interactive = (telegram_token[0] != 0) ? true : interactive;

  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "tg_en", telegram_enabled ? 1 : 0);
    nvs_set_u8(nvs, "tg_inter", telegram_interactive ? 1 : 0);
    nvs_set_str(nvs, "tg_token", telegram_token);
    nvs_set_str(nvs, "tg_chat_id", telegram_chat_id);
    nvs_set_str(nvs, "tg_thresh", telegram_thresholds);
    nvs_set_u32(nvs, "tg_flags", telegram_alert_flags);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  if (policies_updated) {
    meter_save_usage();
  }
}

void config_save_telegram_flags(uint32_t flags) {
  telegram_alert_flags = flags;
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u32(nvs, "tg_flags", telegram_alert_flags);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_wifi(const char *r_ssid, const char *r_pass, bool clear_r_pass,
                      const char *a_ssid, const char *a_pass, uint8_t band_mode, uint8_t proto,
                      bool static_en, const char *ip, const char *mask,
                      const char *gw, const char *dns) {
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) != ESP_OK) return;

  if (r_ssid && r_ssid[0]) {
    nvs_set_str(nvs, "router_ssid", r_ssid);
    strncpy(router_ssid, r_ssid, sizeof(router_ssid) - 1);
  }
  if (clear_r_pass) {
    nvs_set_str(nvs, "router_pass", "");
    router_password[0] = 0;
  } else if (r_pass && r_pass[0]) {
    nvs_set_str(nvs, "router_pass", r_pass);
    strncpy(router_password, r_pass, sizeof(router_password) - 1);
  }

  if (a_ssid && a_ssid[0]) {
    nvs_set_str(nvs, "ap_ssid", a_ssid);
    strncpy(ap_ssid, a_ssid, sizeof(ap_ssid) - 1);
  }
  if (a_pass && strlen(a_pass) >= 8) {
    nvs_set_str(nvs, "ap_pass", a_pass);
    strncpy(ap_password, a_pass, sizeof(ap_password) - 1);
  }

  if (band_mode >= 1 && band_mode <= 3) {
    nvs_set_u8(nvs, "wifi_band", band_mode);
    wifi_band_mode = band_mode;
  }

  nvs_set_u8(nvs, "wifi_proto", proto);
  wifi_protocol_mode = proto;

  nvs_set_u8(nvs, "sta_static", static_en ? 1 : 0);
  sta_static_enabled = static_en;

  if (ip) {
    nvs_set_str(nvs, "sta_ip", ip);
    strncpy(sta_ip_text, ip, sizeof(sta_ip_text) - 1);
  }
  if (mask) {
    nvs_set_str(nvs, "sta_mask", mask);
    strncpy(sta_mask_text, mask, sizeof(sta_mask_text) - 1);
  }
  if (gw) {
    nvs_set_str(nvs, "sta_gw", gw);
    strncpy(sta_gateway_text, gw, sizeof(sta_gateway_text) - 1);
  }
  if (dns) {
    nvs_set_str(nvs, "sta_dns", dns);
    strncpy(sta_dns_text, dns, sizeof(sta_dns_text) - 1);
  }

  nvs_commit(nvs);
  nvs_close(nvs);
}


void config_set_scheduled_reboot_flag(bool flag) {
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "rb_fired", flag ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

bool config_get_scheduled_reboot_flag(void) {
  nvs_handle_t nvs;
  uint8_t flag = 0;
  if (nvs_open("router", NVS_READONLY, &nvs) == ESP_OK) {
    nvs_get_u8(nvs, "rb_fired", &flag);
    nvs_close(nvs);
  }
  return (flag != 0);
}

void config_save_ota_auto_update(bool enabled) {
  ota_auto_update_enabled = enabled;
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "ota_auto", enabled ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}
