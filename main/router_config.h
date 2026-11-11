#pragma once

#include <stdbool.h>
#include <stdint.h>

extern bool has_saved_config;
extern bool sta_static_enabled;
extern char router_ssid[33];
extern char router_password[65];
extern char ap_ssid[33];
extern char ap_password[65];
extern char sta_ip_text[16];
extern char sta_mask_text[16];
extern char sta_gateway_text[16];
extern char sta_dns_text[16];
extern char admin_username[33];
extern char admin_password[65];

extern uint16_t esp_cpu_freq_mhz;
extern bool esp_wifi_power_save;
extern uint8_t wifi_protocol_mode;
extern uint8_t
    wifi_band_mode; // 1 = 2.4GHz Only, 2 = 5GHz Only, 3 = Auto Dual-Band
extern uint8_t router_meter_scale_pct;
extern uint8_t router_meter_scale_mode;

// Daily Scheduled Auto-Reboot Settings
extern bool auto_reboot_enabled;
extern uint8_t auto_reboot_hour;
extern uint8_t auto_reboot_min;
extern int8_t auto_reboot_tz_offset;
extern bool ota_auto_update_enabled;

// Telegram Quota Alerts & Interactive Bot Settings
extern bool telegram_enabled;
extern bool telegram_interactive;
extern char telegram_token[80];
extern char telegram_chat_id[128];
extern char telegram_thresholds[48];
extern uint32_t telegram_alert_flags;

bool telegram_is_admin(const char *chat_id);
bool telegram_broadcast_admins(const char *text);
size_t parse_telegram_thresholds(const char *str, uint8_t out_vals[16]);

void config_load(void);
void config_save_all(void);
void config_save_admin(const char *user, const char *pass);
void config_reset_admin_default(void);
void config_apply_performance(uint16_t freq_mhz, bool wifi_ps);
void config_save_performance(uint16_t freq_mhz, bool wifi_ps);
void config_save_auto_reboot(bool enabled, uint8_t hour, uint8_t min,
                             int8_t tz);
void config_set_scheduled_reboot_flag(bool flag);
bool config_get_scheduled_reboot_flag(void);
void config_save_ota_auto_update(bool enabled);
void config_save_telegram(bool enabled, bool interactive, const char *token,
                          const char *chat_id, const char *thresholds);
void config_save_telegram_flags(uint32_t flags);
void config_save_meter_scale(uint8_t scale_pct, uint16_t gb_std, uint8_t scale_mode);
void config_save_wifi(const char *r_ssid, const char *r_pass, bool clear_r_pass,
                      const char *a_ssid, const char *a_pass, uint8_t band_mode,
                      uint8_t proto, bool static_en, const char *ip,
                      const char *mask, const char *gw, const char *dns);
