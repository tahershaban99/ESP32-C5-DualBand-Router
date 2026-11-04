#include "telegram_bot.h"
#include "router_config.h"
#include "router_globals.h"
#include "meter.h"
#include "ota_updater.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"
#include "lwip/etharp.h"

#define TG_TAG "telegram_bot"

static int64_t last_update_id = 0;
static bool telegram_busy = false;
static SemaphoreHandle_t tg_http_mux = NULL;

static bool tg_http_lock(TickType_t wait_ticks) {
  if (!tg_http_mux) {
    tg_http_mux = xSemaphoreCreateMutex();
  }
  if (!tg_http_mux) return false;
  return (xSemaphoreTake(tg_http_mux, wait_ticks) == pdTRUE);
}

static void tg_http_unlock(void) {
  if (tg_http_mux) {
    xSemaphoreGive(tg_http_mux);
  }
}

// Persistent Interactive Keyboards
static const char *TG_ADMIN_KEYBOARD =
    "{\"keyboard\":[[{\"text\":\"📊 استهلاك الباقة\"},{\"text\":\"👥 الأجهزة المتصلة\"}],"
    "[{\"text\":\"⚡ فحص السرعة\"},{\"text\":\"⚙️ حالة الراوتر\"}],"
    "[{\"text\":\"🚀 تردد المعالج\"},{\"text\":\"🔄 إعادة التشغيل\"}],"
    "[{\"text\":\"📋 مساعدة\"}]],"
    "\"resize_keyboard\":true,\"is_persistent\":true}";

static const char *TG_USER_KEYBOARD =
    "{\"keyboard\":[[{\"text\":\"📊 استهلاك باقتي\"},{\"text\":\"⚡ سرعة اتصالي\"}],"
    "[{\"text\":\"🌐 فحص اتصالي\"},{\"text\":\"📋 مساعدة\"}]],"
    "\"resize_keyboard\":true,\"is_persistent\":true}";

static const char *TG_UNLINKED_KEYBOARD =
    "{\"keyboard\":[[{\"text\":\"🔗 ربط جهازي\"},{\"text\":\"📋 مساعدة\"}]],"
    "\"resize_keyboard\":true,\"is_persistent\":true}";

// Discovered Telegram Users Catalog
static tg_discovered_user_t discovered_tg_users[MAX_DISCOVERED_TG_USERS];
static size_t discovered_tg_users_count = 0;

void telegram_register_discovered_user(const char *chat_id, const char *first_name, const char *username) {
  if (!chat_id || !chat_id[0]) return;
  portENTER_CRITICAL(&data_mux);
  for (size_t i = 0; i < discovered_tg_users_count; ++i) {
    if (strcmp(discovered_tg_users[i].chat_id, chat_id) == 0) {
      if (first_name && first_name[0]) {
        strncpy(discovered_tg_users[i].first_name, first_name, sizeof(discovered_tg_users[i].first_name) - 1);
        discovered_tg_users[i].first_name[sizeof(discovered_tg_users[i].first_name) - 1] = 0;
      }
      if (username && username[0]) {
        strncpy(discovered_tg_users[i].username, username, sizeof(discovered_tg_users[i].username) - 1);
        discovered_tg_users[i].username[sizeof(discovered_tg_users[i].username) - 1] = 0;
      }
      discovered_tg_users[i].last_seen_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
      portEXIT_CRITICAL(&data_mux);
      return;
    }
  }

  if (discovered_tg_users_count < MAX_DISCOVERED_TG_USERS) {
    size_t idx = discovered_tg_users_count++;
    memset(&discovered_tg_users[idx], 0, sizeof(tg_discovered_user_t));
    strncpy(discovered_tg_users[idx].chat_id, chat_id, sizeof(discovered_tg_users[idx].chat_id) - 1);
    if (first_name && first_name[0]) {
      strncpy(discovered_tg_users[idx].first_name, first_name, sizeof(discovered_tg_users[idx].first_name) - 1);
    }
    if (username && username[0]) {
      strncpy(discovered_tg_users[idx].username, username, sizeof(discovered_tg_users[idx].username) - 1);
    }
    discovered_tg_users[idx].last_seen_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  }
  portEXIT_CRITICAL(&data_mux);
}

size_t telegram_get_discovered_users(tg_discovered_user_t *out_users, size_t max_count) {
  if (!out_users || max_count == 0) return 0;
  size_t cnt = 0;
  portENTER_CRITICAL(&data_mux);
  cnt = (discovered_tg_users_count < max_count) ? discovered_tg_users_count : max_count;
  memcpy(out_users, discovered_tg_users, cnt * sizeof(tg_discovered_user_t));
  portEXIT_CRITICAL(&data_mux);
  return cnt;
}

bool telegram_is_admin(const char *chat_id) {
  if (!chat_id || !chat_id[0] || !telegram_chat_id[0]) return false;
  char copy[140];
  strncpy(copy, telegram_chat_id, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = 0;
  char *saveptr = NULL;
  char *tok = strtok_r(copy, ", ;", &saveptr);
  while (tok) {
    while (*tok == ' ') tok++;
    if (strcmp(tok, chat_id) == 0) return true;
    tok = strtok_r(NULL, ", ;", &saveptr);
  }
  return false;
}

void telegram_broadcast_admins(const char *text) {
  if (!text || !text[0] || !telegram_chat_id[0]) return;
  char copy[140];
  strncpy(copy, telegram_chat_id, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = 0;
  char *saveptr = NULL;
  char *tok = strtok_r(copy, ", ;", &saveptr);
  while (tok) {
    while (*tok == ' ') tok++;
    if (*tok) {
      telegram_send_message_with_keyboard(tok, text, TG_ADMIN_KEYBOARD);
    }
    tok = strtok_r(NULL, ", ;", &saveptr);
  }
}

// Helper to escape characters for JSON string value
static void json_escape(const char *src, char *dst, size_t max_len) {
  if (!src || !dst || max_len == 0) return;
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 4 < max_len; ++i) {
    if (src[i] == '"') { dst[j++] = '\\'; dst[j++] = '"'; }
    else if (src[i] == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
    else if (src[i] == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
    else if (src[i] == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
    else if (src[i] == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
    else dst[j++] = src[i];
  }
  dst[j] = 0;
}

bool telegram_send_message(const char *chat_id, const char *text) {
  return telegram_send_message_with_keyboard(chat_id, text, NULL);
}

bool telegram_send_message_with_keyboard(const char *chat_id, const char *text, const char *inline_keyboard_json) {
  if (!telegram_token[0] || !chat_id || !chat_id[0] || !text || !text[0]) {
    return false;
  }

  if (!tg_http_lock(pdMS_TO_TICKS(15000))) {
    ESP_LOGW(TG_TAG, "Cannot send message: Telegram HTTP client locked by another task");
    return false;
  }

  char url[180];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", telegram_token);

  char *escaped_text = (char *)malloc(2048);
  if (!escaped_text) {
    ESP_LOGE(TG_TAG, "Failed to allocate memory for escaped_text");
    tg_http_unlock();
    return false;
  }
  json_escape(text, escaped_text, 2048);

  char *body = (char *)malloc(2800);
  if (!body) {
    ESP_LOGE(TG_TAG, "Failed to allocate memory for sendMessage body");
    free(escaped_text);
    tg_http_unlock();
    return false;
  }

  if (inline_keyboard_json && inline_keyboard_json[0]) {
    snprintf(body, 2800,
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\",\"reply_markup\":%s}",
             chat_id, escaped_text, inline_keyboard_json);
  } else {
    snprintf(body, 2800,
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\"}",
             chat_id, escaped_text);
  }
  free(escaped_text);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 10000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 512,
      .buffer_size_tx = 2800,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TG_TAG, "Failed to initialize HTTP client for sendMessage");
    free(body);
    tg_http_unlock();
    return false;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, strlen(body));

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  free(body);
  tg_http_unlock();

  if (err == ESP_OK && (status == 200 || status == 201)) {
    ESP_LOGI(TG_TAG, "Telegram message sent successfully to %s", chat_id);
    return true;
  } else {
    ESP_LOGW(TG_TAG, "Failed to send Telegram message to %s: err=%s, status=%d", chat_id, esp_err_to_name(err), status);
    return false;
  }
}

bool telegram_answer_callback_query(const char *callback_query_id, const char *text) {
  if (!telegram_token[0] || !callback_query_id || !callback_query_id[0]) return false;

  if (!tg_http_lock(pdMS_TO_TICKS(8000))) {
    return false;
  }

  char url[180];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/answerCallbackQuery", telegram_token);

  char body[256];
  if (text && text[0]) {
    char esc_txt[100];
    json_escape(text, esc_txt, sizeof(esc_txt));
    snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\",\"text\":\"%s\"}", callback_query_id, esc_txt);
  } else {
    snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\"}", callback_query_id);
  }

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 6000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 256,
      .buffer_size_tx = 300,
  };
 
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    tg_http_unlock();
    return false;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, strlen(body));

  esp_err_t err = esp_http_client_perform(client);
  esp_http_client_cleanup(client);
  tg_http_unlock();
  return (err == ESP_OK);
}

int telegram_send_test_alert_diag(char *out_err, size_t max_err) {
  if (!telegram_token[0] || !telegram_chat_id[0]) {
    if (out_err && max_err > 0) {
      snprintf(out_err, max_err, "لم يتم إدخال توكن البوت أو معرف المشرفين Chat ID!");
    }
    return -1;
  }

  // Check STA connection
  wifi_ap_record_t ap_rec;
  if (esp_wifi_sta_get_ap_info(&ap_rec) != ESP_OK) {
    if (out_err && max_err > 0) {
      snprintf(out_err, max_err, "الراوتر غير متصل بالإنترنت حالياً (STA غير متصل). يرجى ربطه بالواي فاي المنزلي أولاً.");
    }
    return -2;
  }

  if (!tg_http_lock(pdMS_TO_TICKS(15000))) {
    if (out_err && max_err > 0) {
      snprintf(out_err, max_err, "محرك التيليجرام مشغول حالياً، يرجى إعادة المحاولة بعد لحظات.");
    }
    return -5;
  }

  // Extract first admin ID
  char first_admin[48] = {0};
  char copy[140];
  strncpy(copy, telegram_chat_id, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = 0;
  char *saveptr = NULL;
  char *tok = strtok_r(copy, ", ;", &saveptr);
  if (tok) {
    while (*tok == ' ') tok++;
    strncpy(first_admin, tok, sizeof(first_admin) - 1);
  }
  if (!first_admin[0]) {
    tg_http_unlock();
    if (out_err && max_err > 0) {
      snprintf(out_err, max_err, "معرف المشرف Chat ID غير صالح!");
    }
    return -1;
  }

  char url[180];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", telegram_token);

  const char *msg = "✅ <b>اختبار إشعارات راوتر ESP32 المتقدم</b>\n\n"
                    "الاتصال ببوت التيليجرام يعمل بنجاح وبكفاءة فائقة! 🚀\n"
                    "ستصلك الآن تنبيهات الاستهلاك التلقائية للراوتر ولأجهزتك فور تجاوز الحدود المقررة.\n"
                    "💡 تم تفعيل لوحة المفاتيح التفاعلية أدناه لتسهيل الاستعلام والتحكم بلمسة واحدة.";

  char *escaped_text = (char *)malloc(2048);
  if (!escaped_text) {
    tg_http_unlock();
    if (out_err && max_err > 0) snprintf(out_err, max_err, "فشل تخصيص الذاكرة");
    return -2;
  }
  json_escape(msg, escaped_text, 2048);

  char *body = (char *)malloc(2800);
  if (!body) {
    free(escaped_text);
    tg_http_unlock();
    if (out_err && max_err > 0) snprintf(out_err, max_err, "فشل تخصيص الذاكرة");
    return -2;
  }
  snprintf(body, 2800,
           "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\",\"reply_markup\":%s}",
           first_admin, escaped_text, TG_ADMIN_KEYBOARD);
  free(escaped_text);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 10000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 512,
      .buffer_size_tx = 2400,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    free(body);
    tg_http_unlock();
    if (out_err && max_err > 0) {
      snprintf(out_err, max_err, "فشل إنشاء عميل الاتصال HTTP");
    }
    return -3;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, strlen(body));

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  free(body);
  tg_http_unlock();

  if (err == ESP_OK && (status == 200 || status == 201)) {
    if (out_err && max_err > 0) {
      snprintf(out_err, max_err, "تم إرسال التنبيه التجريبي عبر التيليجرام بنجاح! تفقد محادثتك.");
    }
    return 0;
  }

  if (out_err && max_err > 0) {
    if (err != ESP_OK) {
      snprintf(out_err, max_err, "فشل الاتصال بخادم تيليجرام (%s). تأكد من توفر الإنترنت والـ DNS.", esp_err_to_name(err));
    } else if (status == 401) {
      snprintf(out_err, max_err, "توكن البوت غير صحيح (401 Unauthorized)، يرجى التأكد من نسخه بالكامل من @BotFather.");
    } else if (status == 400 || status == 403) {
      snprintf(out_err, max_err, "معرف Chat ID غير صحيح أو أنك لم تبدأ المحادثة بالضغط على Start في البوت (كود %d).", status);
    } else {
      snprintf(out_err, max_err, "رفض تيليجرام إرسال الرسالة، رمز الاستجابة: %d.", status);
    }
  }
  return status > 0 ? status : -4;
}

bool telegram_send_test_alert(void) {
  return (telegram_send_test_alert_diag(NULL, 0) == 0);
}

void telegram_reset_alert_flags(void) {
  telegram_alert_flags = 0;
  config_save_telegram_flags(0);
  ESP_LOGI(TG_TAG, "Telegram alert threshold flags reset for new billing cycle.");
}

// -----------------------------------------------------------------------------
// Helper to find device MAC & info from IP string or partial name
// -----------------------------------------------------------------------------
static bool find_device_by_target(const char *target, uint8_t out_mac[6], char *out_name, size_t max_name, uint32_t *out_ip) {
  if (!target || !target[0]) return false;
  while (*target == ' ') target++;

  // 1. Check if target is an IPv4 string (e.g. 192.168.4.2)
  ip4_addr_t ip_val;
  if (ip4addr_aton(target, &ip_val)) {
    uint32_t req_ip = ip_val.addr;
    if (out_ip) *out_ip = req_ip;

    portENTER_CRITICAL(&data_mux);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
      if (active_clients[i].ip == req_ip && valid_device_mac(active_clients[i].mac)) {
        memcpy(out_mac, active_clients[i].mac, 6);
        int n = meter_find_name_locked(out_mac, false);
        if (n >= 0 && device_names[n].name[0]) {
          snprintf(out_name, max_name, "%s", device_names[n].name);
        } else {
          snprintf(out_name, max_name, "%s", target);
        }
        portEXIT_CRITICAL(&data_mux);
        return true;
      }
    }
    for (size_t b = 0; b < binding_count; ++b) {
      if (device_bindings[b].bound_ip == req_ip && valid_device_mac(device_bindings[b].mac)) {
        memcpy(out_mac, device_bindings[b].mac, 6);
        int n = meter_find_name_locked(out_mac, false);
        if (n >= 0 && device_names[n].name[0]) {
          snprintf(out_name, max_name, "%s", device_names[n].name);
        } else {
          snprintf(out_name, max_name, "%s", target);
        }
        portEXIT_CRITICAL(&data_mux);
        return true;
      }
    }
    // Fallback: check lwIP ARP cache
    ip4_addr_t ip_addr = {.addr = req_ip};
    struct eth_addr *eth_ret = NULL;
    const ip4_addr_t *ip_ret = NULL;
    if (etharp_find_addr(NULL, &ip_addr, &eth_ret, &ip_ret) >= 0 && eth_ret != NULL) {
      if (valid_device_mac(eth_ret->addr)) {
        memcpy(out_mac, eth_ret->addr, 6);
        snprintf(out_name, max_name, "%s", target);
        portEXIT_CRITICAL(&data_mux);
        return true;
      }
    }
    portEXIT_CRITICAL(&data_mux);
    return false;
  }

  // 2. Check if target matches device name
  portENTER_CRITICAL(&data_mux);
  for (size_t n = 0; n < name_count; ++n) {
    if (strstr(device_names[n].name, target) != NULL) {
      memcpy(out_mac, device_names[n].mac, 6);
      snprintf(out_name, max_name, "%s", device_names[n].name);
      if (out_ip) {
        *out_ip = 0;
        for (size_t k = 0; k < MAX_CLIENTS; ++k) {
          if (memcmp(active_clients[k].mac, out_mac, 6) == 0) {
            *out_ip = active_clients[k].ip;
            break;
          }
        }
      }
      portEXIT_CRITICAL(&data_mux);
      return true;
    }
  }
  portEXIT_CRITICAL(&data_mux);
  return false;
}

// -----------------------------------------------------------------------------
// Formatters for Single Device and Multi-Device Reports
// -----------------------------------------------------------------------------
static void format_single_device_report(int p_idx, char *out, size_t max_out) {
  if (p_idx < 0 || (size_t)p_idx >= policy_count) {
    snprintf(out, max_out, "⚠️ بيانات الجهاز غير متوفرة حالياً.");
    return;
  }
  uint8_t mac[6];
  uint64_t quota = 0;
  bool blk = false, gam = false;
  char dev_name[DEVICE_NAME_BYTES] = "جهاز متصل";

  portENTER_CRITICAL(&data_mux);
  memcpy(mac, device_policies[p_idx].mac, 6);
  quota = device_policies[p_idx].quota_bytes;
  blk = device_policies[p_idx].blocked;
  gam = device_policies[p_idx].gaming_mode;

  int n = meter_find_name_locked(mac, false);
  if (n >= 0 && device_names[n].name[0]) {
    strncpy(dev_name, device_names[n].name, sizeof(dev_name) - 1);
    dev_name[sizeof(dev_name) - 1] = 0;
  }

  uint32_t ip = 0;
  bool connected = false;
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (memcmp(active_clients[k].mac, mac, 6) == 0) {
      ip = active_clients[k].ip;
      connected = (active_clients[k].aid != 0);
      break;
    }
  }
  if (ip == 0) {
    for (size_t b = 0; b < binding_count; ++b) {
      if (memcmp(device_bindings[b].mac, mac, 6) == 0) {
        ip = device_bindings[b].bound_ip;
        break;
      }
    }
  }

  int u = meter_find_usage_locked(mac, false);
  uint64_t up = (u >= 0) ? usage_records[u].upload_bytes : 0;
  uint64_t down = (u >= 0) ? usage_records[u].download_bytes : 0;
  uint32_t live_down = (u >= 0) ? device_live_speeds[u].cur_down_bps : 0;
  uint32_t live_up = (u >= 0) ? device_live_speeds[u].cur_up_bps : 0;

  int s = meter_find_speed_locked(mac, false);
  uint32_t cap_up = (s >= 0) ? device_speeds[s].upload_bps : 0;
  uint32_t cap_down = (s >= 0) ? device_speeds[s].download_bps : 0;
  portEXIT_CRITICAL(&data_mux);

  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  double up_gb = (double)up / gb_div;
  double down_gb = (double)down / gb_div;
  double tot_gb = up_gb + down_gb;
  double q_gb = (double)quota / gb_div;
  double rem_gb = (q_gb > tot_gb) ? (q_gb - tot_gb) : 0.0;
  uint32_t pct = (quota > 0) ? (uint32_t)((tot_gb / q_gb) * 100.0) : 0;
  if (pct > 100) pct = 100;

  ip4_addr_t tip = {.addr = ip};
  char ip_str[24] = "--";
  if (ip != 0) snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", IP2STR(&tip));

  const char *conn_str = blk ? "🔴 محظور من الإنترنت" : (connected ? "🟢 متصل بالواي فاي" : "⚪ غير متصل حالياً");

  char cap_str[90] = "غير محدود ♾️";
  if (cap_down > 0 || cap_up > 0) {
    snprintf(cap_str, sizeof(cap_str), "⬇️ %.1f Mbps | ⬆️ %.1f Mbps",
             (double)cap_down / 1000000.0, (double)cap_up / 1000000.0);
  }

  char q_str[130] = "غير محددة (استهلاك مفتوح ♾️)";
  if (quota > 0) {
    snprintf(q_str, sizeof(q_str), "%.2f GB (متبقي %.2f GB - %u%%)", q_gb, rem_gb, (unsigned)pct);
  }

  snprintf(out, max_out,
           "📱 <b>تقرير جهازك: %s</b>\n\n"
           "🌐 <b>عنوان IP:</b> <code>%s</code>\n"
           "📶 <b>حالة الاتصال:</b> %s%s\n"
           "📊 <b>حصة الباقة:</b> %s\n"
           "📥 <b>إجمالي الاستهلاك:</b> %.2f GB\n"
           "   • ⬇️ تحميل: %.2f GB\n"
           "   • ⬆️ رفع: %.2f GB\n"
           "⚡ <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n"
           "🛡️ <b>سقف السرعة:</b> %s",
           dev_name, ip_str, conn_str, gam ? " | 🎮 VIP" : "",
           q_str, tot_gb, down_gb, up_gb,
           (double)live_down / 1000000.0, (double)live_up / 1000000.0,
           cap_str);
}

static void format_multi_device_summary(const int *p_indices, size_t count, char *out, size_t max_out, char *kb_out, size_t max_kb) {
  uint64_t total_used = 0;
  uint64_t total_quota = 0;
  uint64_t total_up = 0;
  uint64_t total_down = 0;

  char dev_lines[700] = {0};
  char kb_buttons[500] = {0};
  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);

  for (size_t i = 0; i < count; ++i) {
    int p_idx = p_indices[i];
    if (p_idx < 0 || (size_t)p_idx >= policy_count) continue;

    uint8_t mac[6];
    uint64_t quota = 0;
    bool blk = false;
    char dev_name[DEVICE_NAME_BYTES] = "جهاز";

    portENTER_CRITICAL(&data_mux);
    memcpy(mac, device_policies[p_idx].mac, 6);
    quota = device_policies[p_idx].quota_bytes;
    blk = device_policies[p_idx].blocked;

    int n = meter_find_name_locked(mac, false);
    if (n >= 0 && device_names[n].name[0]) {
      strncpy(dev_name, device_names[n].name, sizeof(dev_name) - 1);
      dev_name[sizeof(dev_name) - 1] = 0;
    }

    int u = meter_find_usage_locked(mac, false);
    uint64_t up = (u >= 0) ? usage_records[u].upload_bytes : 0;
    uint64_t down = (u >= 0) ? usage_records[u].download_bytes : 0;
    portEXIT_CRITICAL(&data_mux);

    uint64_t dev_tot = up + down;
    total_used += dev_tot;
    total_up += up;
    total_down += down;
    total_quota += quota;

    double d_used_gb = (double)dev_tot / gb_div;
    double d_q_gb = (double)quota / gb_div;
    uint32_t pct = (quota > 0) ? (uint32_t)((d_used_gb / d_q_gb) * 100.0) : 0;

    char line[160];
    if (quota > 0) {
      snprintf(line, sizeof(line), "• <b>%s</b>: %.2f / %.2f GB (%u%%) %s\n",
               dev_name, d_used_gb, d_q_gb, (unsigned)pct, blk ? "🔴" : "🟢");
    } else {
      snprintf(line, sizeof(line), "• <b>%s</b>: %.2f GB %s\n",
               dev_name, d_used_gb, blk ? "🔴" : "🟢");
    }
    if (strlen(dev_lines) + strlen(line) < sizeof(dev_lines) - 20) {
      strcat(dev_lines, line);
    }

    // Add inline keyboard button
    char btn[140];
    snprintf(btn, sizeof(btn), "%s[{\"text\":\"📱 %s\",\"callback_data\":\"dev_idx:%d\"}]",
             (kb_buttons[0] ? "," : ""), dev_name, p_idx);
    if (strlen(kb_buttons) + strlen(btn) < sizeof(kb_buttons) - 30) {
      strcat(kb_buttons, btn);
    }
  }

  double tot_used_gb = (double)total_used / gb_div;
  double tot_up_gb = (double)total_up / gb_div;
  double tot_down_gb = (double)total_down / gb_div;
  double tot_q_gb = (double)total_quota / gb_div;
  double rem_q_gb = (tot_q_gb > tot_used_gb) ? (tot_q_gb - tot_used_gb) : 0.0;
  uint32_t overall_pct = (tot_q_gb > 0) ? (uint32_t)((tot_used_gb / tot_q_gb) * 100.0) : 0;

  char q_header[180] = "";
  if (tot_q_gb > 0) {
    snprintf(q_header, sizeof(q_header),
             "🎯 <b>إجمالي الباقات:</b> %.2f GB (متبقي %.2f GB - %u%%)\n",
             tot_q_gb, rem_q_gb, (unsigned)overall_pct);
  }

  snprintf(out, max_out,
           "📊 <b>تقرير استهلاك أجهزتك المربوطة (%u أجهزة):</b>\n\n"
           "📥 <b>إجمالي الاستهلاك:</b> %.2f GB (⬇️ %.2f GB | ⬆️ %.2f GB)\n"
           "%s\n"
           "📱 <b>قائمة الأجهزة:</b>\n"
           "%s\n"
           "👇 <i>اضغط على زر أي جهاز بالأسفل لعرض تفاصيله وسرعته:</i>",
           (unsigned)count, tot_used_gb, tot_down_gb, tot_up_gb,
           q_header, dev_lines);

  // Add refresh button
  if (strlen(kb_buttons) + 60 < sizeof(kb_buttons)) {
    strcat(kb_buttons, ",[{\"text\":\"🔄 تحديث البيانات\",\"callback_data\":\"dev_all\"}]");
  }

  snprintf(kb_out, max_kb, "{\"inline_keyboard\":[%s]}", kb_buttons);
}

// -----------------------------------------------------------------------------
// Automated Multi-Threshold Push Notifications (Main Quota & Per-Device Quota)
// -----------------------------------------------------------------------------
void telegram_check_quota_alerts(void) {
  if (!telegram_enabled || !telegram_token[0]) {
    return;
  }
  if (telegram_busy) return;

  // Only check and send alerts if STA is actively connected to internet
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
    return;
  }

  uint64_t total_used_bytes = 0;
  uint64_t main_quota_bytes_val = 0;
  portENTER_CRITICAL(&data_mux);
  for (size_t i = 0; i < usage_count; ++i) {
    total_used_bytes += (usage_records[i].upload_bytes + usage_records[i].download_bytes);
  }
  main_quota_bytes_val = main_quota_bytes;
  portEXIT_CRITICAL(&data_mux);

  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);

  // 1. Check Main ISP Quota Multi-Thresholds
  if (main_quota_bytes_val > 0 && telegram_chat_id[0]) {
    uint32_t current_pct = (uint32_t)((total_used_bytes * 100ULL) / main_quota_bytes_val);

    char thresh_copy[50];
    strncpy(thresh_copy, telegram_thresholds, sizeof(thresh_copy) - 1);
    thresh_copy[sizeof(thresh_copy) - 1] = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(thresh_copy, ",", &saveptr);
    uint8_t th_idx = 0;

    while (tok && th_idx < 16) {
      while (*tok == ' ') tok++;
      int th_val = atoi(tok);
      if (th_val > 0 && th_val <= 100) {
        uint32_t mask = (1UL << th_idx);
        if (current_pct >= (uint32_t)th_val && !(telegram_alert_flags & mask)) {
          telegram_alert_flags |= mask;
          config_save_telegram_flags(telegram_alert_flags);

          double used_gb = (double)total_used_bytes / gb_div;
          double total_gb = (double)main_quota_bytes_val / gb_div;
          double rem_gb = total_gb > used_gb ? (total_gb - used_gb) : 0.0;

          time_t now = time(NULL);
          int32_t rem_days = -1, rem_hours = -1;
          if (now > 1700000000 && quota_end_date[0]) {
            time_t end_ts = meter_parse_date(quota_end_date, true);
            if (end_ts > now) {
              int64_t diff = (int64_t)(end_ts - now);
              rem_days = (int32_t)(diff / 86400);
              rem_hours = (int32_t)((diff % 86400) / 3600);
            } else {
              rem_days = 0; rem_hours = 0;
            }
          }

          uint16_t active_cnt = 0;
          portENTER_CRITICAL(&data_mux);
          for (size_t k = 0; k < MAX_CLIENTS; ++k) {
            if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
              active_cnt++;
            }
          }
          portEXIT_CRITICAL(&data_mux);

          char alert_msg[550];
          if (rem_days >= 0) {
            snprintf(alert_msg, sizeof(alert_msg),
                     "🚨 <b>تنبيه استهلاك باقة الإنترنت الرئيسية (%u%%)</b> 🚨\n\n"
                     "📊 <b>النسبة المستهلكة:</b> %u%%\n"
                     "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB\n"
                     "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
                     "📅 <b>تاريخ التجديد:</b> %s (متبقي %ld يوم و %ld ساعة)\n"
                     "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل",
                     th_val, (unsigned)current_pct, used_gb, total_gb, rem_gb,
                     quota_end_date, (long)rem_days, (long)rem_hours, active_cnt);
          } else {
            snprintf(alert_msg, sizeof(alert_msg),
                     "🚨 <b>تنبيه استهلاك باقة الإنترنت الرئيسية (%u%%)</b> 🚨\n\n"
                     "📊 <b>النسبة المستهلكة:</b> %u%%\n"
                     "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB\n"
                     "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
                     "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل",
                     th_val, (unsigned)current_pct, used_gb, total_gb, rem_gb, active_cnt);
          }

          ESP_LOGW(TG_TAG, "Main ISP threshold %u%% triggered! Sending alert to admins...", th_val);
          telegram_busy = true;
          telegram_broadcast_admins(alert_msg);
          telegram_busy = false;
          return; // Send one alert per check
        }
      }
      tok = strtok_r(NULL, ",", &saveptr);
      th_idx++;
    }
  }

  // 2. Check Per-Device Quota Multi-Thresholds (Clean, Deadlock-Free Snapshot)
  for (size_t p = 0; p < MAX_RECORDS; ++p) {
    device_policy_t pol;
    char dev_name[DEVICE_NAME_BYTES] = "جهاز متصل";
    uint32_t dev_ip = 0;
    uint64_t dev_used = 0;
    bool has_policy = false;

    portENTER_CRITICAL(&data_mux);
    if (p < policy_count && device_policies[p].quota_bytes > 0) {
      pol = device_policies[p];
      has_policy = true;

      int u = meter_find_usage_locked(pol.mac, false);
      if (u >= 0) {
        dev_used = usage_records[u].upload_bytes + usage_records[u].download_bytes;
      }
      int n_idx = meter_find_name_locked(pol.mac, false);
      if (n_idx >= 0 && device_names[n_idx].name[0]) {
        strncpy(dev_name, device_names[n_idx].name, sizeof(dev_name) - 1);
        dev_name[sizeof(dev_name) - 1] = 0;
      }
      for (size_t k = 0; k < MAX_CLIENTS; ++k) {
        if (memcmp(active_clients[k].mac, pol.mac, 6) == 0 && active_clients[k].aid != 0) {
          dev_ip = active_clients[k].ip;
          break;
        }
      }
      if (dev_ip == 0) {
        int b = meter_find_binding_locked(pol.mac, false);
        if (b >= 0) dev_ip = device_bindings[b].bound_ip;
      }
    }
    portEXIT_CRITICAL(&data_mux);

    if (!has_policy) continue;

    uint32_t dev_pct = (uint32_t)((dev_used * 100ULL) / pol.quota_bytes);

    char thresh_copy[50];
    strncpy(thresh_copy, telegram_thresholds, sizeof(thresh_copy) - 1);
    thresh_copy[sizeof(thresh_copy) - 1] = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(thresh_copy, ",", &saveptr);
    uint8_t th_idx = 0;

    while (tok && th_idx < 16) {
      while (*tok == ' ') tok++;
      int th_val = atoi(tok);
      if (th_val > 0 && th_val <= 100) {
        uint16_t mask = (1U << th_idx);
        if (dev_pct >= (uint32_t)th_val && !(pol.alert_flags & mask)) {
          portENTER_CRITICAL(&data_mux);
          int p_cur = meter_find_policy_locked(pol.mac, false);
          if (p_cur >= 0) {
            device_policies[p_cur].alert_flags |= mask;
          }
          portEXIT_CRITICAL(&data_mux);

          double dev_used_gb = (double)dev_used / gb_div;
          double dev_total_gb = (double)pol.quota_bytes / gb_div;
          double dev_rem_gb = (dev_total_gb > dev_used_gb) ? (dev_total_gb - dev_used_gb) : 0.0;

          char alert_msg[550];
          ip4_addr_t dev_addr = {.addr = dev_ip};
          const char *status_str = (dev_pct >= 100) ? "🚫 تم حظر الوصول للإنترنت تلقائياً لوصول الاستهلاك إلى 100%" : "🟢 متصل وتحت المراقبة";

          snprintf(alert_msg, sizeof(alert_msg),
                   "🚨 <b>تنبيه استهلاك باقة الجهاز (%u%%)</b> 🚨\n\n"
                   "📱 <b>الجهاز:</b> %s\n"
                   "🌐 <b>عنوان IP:</b> %d.%d.%d.%d\n"
                   "📊 <b>النسبة المستهلكة:</b> %u%%\n"
                   "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB\n"
                   "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
                   "⚠️ <b>الحالة:</b> %s",
                   th_val, dev_name, IP2STR(&dev_addr), (unsigned)dev_pct,
                   dev_used_gb, dev_total_gb, dev_rem_gb, status_str);

          ESP_LOGW(TG_TAG, "Device alert triggered: %s reached %u%%", dev_name, th_val);
          telegram_busy = true;

          // Dispatch to linked owner with TG_USER_KEYBOARD, and CC to all admins
          if (pol.telegram_chat_id[0]) {
            telegram_send_message_with_keyboard(pol.telegram_chat_id, alert_msg, TG_USER_KEYBOARD);
            char cc_msg[650];
            snprintf(cc_msg, sizeof(cc_msg), "📢 <b>[تنبيه إداري: استهلاك باقة جهاز (%u%%)]</b>\n\n%s", th_val, alert_msg);
            telegram_broadcast_admins(cc_msg);
          } else {
            // No personal chat_id: alert all admins
            telegram_broadcast_admins(alert_msg);
          }

          telegram_busy = false;
          meter_save_usage();
          return; // Send one alert per check
        }
      }
      tok = strtok_r(NULL, ",", &saveptr);
      th_idx++;
    }
  }
}

// -----------------------------------------------------------------------------
// Telegram JSON Unicode & Escape Sequence Decoder (RFC 8259 Compliant)
// -----------------------------------------------------------------------------
static int hex_char_to_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void json_decode_string(const char *src, char *dst, size_t max_dst) {
  if (!src || !dst || max_dst == 0) return;
  size_t d = 0;
  size_t s = 0;

  while (src[s] && src[s] != '"' && d + 4 < max_dst) {
    if (src[s] == '\\' && src[s + 1]) {
      char c = src[s + 1];
      if (c == 'u' && src[s + 2] && src[s + 3] && src[s + 4] && src[s + 5]) {
        int h0 = hex_char_to_val(src[s + 2]);
        int h1 = hex_char_to_val(src[s + 3]);
        int h2 = hex_char_to_val(src[s + 4]);
        int h3 = hex_char_to_val(src[s + 5]);
        if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
          uint32_t val = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
          s += 6;

          // Check UTF-16 surrogate pair (for 4-byte Emojis: 0xD800-0xDBFF followed by 0xDC00-0xDFFF)
          if (val >= 0xD800 && val <= 0xDBFF && src[s] == '\\' && src[s + 1] == 'u' &&
              src[s + 2] && src[s + 3] && src[s + 4] && src[s + 5]) {
            int l0 = hex_char_to_val(src[s + 2]);
            int l1 = hex_char_to_val(src[s + 3]);
            int l2 = hex_char_to_val(src[s + 4]);
            int l3 = hex_char_to_val(src[s + 5]);
            if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
              uint32_t val2 = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
              if (val2 >= 0xDC00 && val2 <= 0xDFFF) {
                val = 0x10000 + ((val - 0xD800) << 10) + (val2 - 0xDC00);
                s += 6;
              }
            }
          }

          // Encode Unicode codepoint to valid UTF-8
          if (val <= 0x7F) {
            dst[d++] = (char)val;
          } else if (val <= 0x7FF) {
            dst[d++] = (char)(0xC0 | (val >> 6));
            dst[d++] = (char)(0x80 | (val & 0x3F));
          } else if (val <= 0xFFFF) {
            dst[d++] = (char)(0xE0 | (val >> 12));
            dst[d++] = (char)(0x80 | ((val >> 6) & 0x3F));
            dst[d++] = (char)(0x80 | (val & 0x3F));
          } else if (val <= 0x10FFFF) {
            dst[d++] = (char)(0xF0 | (val >> 18));
            dst[d++] = (char)(0x80 | ((val >> 12) & 0x3F));
            dst[d++] = (char)(0x80 | ((val >> 6) & 0x3F));
            dst[d++] = (char)(0x80 | (val & 0x3F));
          }
          continue;
        }
      }
      // Standard escape sequences
      if (c == '"') { dst[d++] = '"'; s += 2; }
      else if (c == '\\') { dst[d++] = '\\'; s += 2; }
      else if (c == '/') { dst[d++] = '/'; s += 2; }
      else if (c == 'b') { dst[d++] = '\b'; s += 2; }
      else if (c == 'f') { dst[d++] = '\f'; s += 2; }
      else if (c == 'n') { dst[d++] = '\n'; s += 2; }
      else if (c == 'r') { dst[d++] = '\r'; s += 2; }
      else if (c == 't') { dst[d++] = '\t'; s += 2; }
      else { dst[d++] = c; s += 2; }
    } else {
      dst[d++] = src[s++];
    }
  }
  dst[d] = '\0';
}

// -----------------------------------------------------------------------------
// Interactive Report Generators (Linked Users & Network Administrators)
// -----------------------------------------------------------------------------
static void send_user_quota_report(const char *chat_id, const int *linked_indices, size_t linked_count) {
  if (linked_count == 1) {
    char dev_rep[950];
    format_single_device_report(linked_indices[0], dev_rep, sizeof(dev_rep));
    const char *kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 تحديث استهلاك باقتي\",\"callback_data\":\"user_quota\"}]]}";
    telegram_send_message_with_keyboard(chat_id, dev_rep, kb);
  } else if (linked_count > 1) {
    char sum_rep[1000], kb_rep[600];
    format_multi_device_summary(linked_indices, linked_count, sum_rep, sizeof(sum_rep), kb_rep, sizeof(kb_rep));
    telegram_send_message_with_keyboard(chat_id, sum_rep, kb_rep);
  }
}

static void send_user_speed_report(const char *chat_id, const int *linked_indices, size_t linked_count) {
  char spd_rep[750] = "⚡ <b>تقرير سرعة أجهزتك اللحظية:</b>\n\n";
  portENTER_CRITICAL(&data_mux);
  for (size_t li = 0; li < linked_count; ++li) {
    int p_idx = linked_indices[li];
    uint8_t mac[6];
    memcpy(mac, device_policies[p_idx].mac, 6);
    char d_name[DEVICE_NAME_BYTES] = "جهاز";
    int n = meter_find_name_locked(mac, false);
    if (n >= 0 && device_names[n].name[0]) {
      strncpy(d_name, device_names[n].name, sizeof(d_name) - 1);
    }
    int u = meter_find_usage_locked(mac, false);
    uint32_t c_down = (u >= 0) ? device_live_speeds[u].cur_down_bps : 0;
    uint32_t c_up = (u >= 0) ? device_live_speeds[u].cur_up_bps : 0;
    int s = meter_find_speed_locked(mac, false);
    uint32_t cap_down = (s >= 0) ? device_speeds[s].download_bps : 0;
    uint32_t cap_up = (s >= 0) ? device_speeds[s].upload_bps : 0;

    char lbuf[180];
    if (cap_down > 0 || cap_up > 0) {
      snprintf(lbuf, sizeof(lbuf),
               "• <b>%s</b>:\n   ⬇️ %.2f Mbps (حد: %.1f M) | ⬆️ %.2f Mbps (حد: %.1f M)\n",
               d_name, (double)c_down / 1000000.0, (double)cap_down / 1000000.0,
               (double)c_up / 1000000.0, (double)cap_up / 1000000.0);
    } else {
      snprintf(lbuf, sizeof(lbuf),
               "• <b>%s</b>:\n   ⬇️ %.2f Mbps | ⬆️ %.2f Mbps (السرعة كاملة مفتوحة 🚀)\n",
               d_name, (double)c_down / 1000000.0, (double)c_up / 1000000.0);
    }
    if (strlen(spd_rep) + strlen(lbuf) < sizeof(spd_rep) - 20) strcat(spd_rep, lbuf);
  }
  portEXIT_CRITICAL(&data_mux);

  const char *kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 فحص وتحديث السرعة\",\"callback_data\":\"user_speed\"}]]}";
  telegram_send_message_with_keyboard(chat_id, spd_rep, kb);
}

static void send_user_status_report(const char *chat_id, const int *linked_indices, size_t linked_count) {
  char st_rep[750] = "🌐 <b>فحص حالة اتصال أجهزتك:</b>\n\n";
  portENTER_CRITICAL(&data_mux);
  for (size_t li = 0; li < linked_count; ++li) {
    int p_idx = linked_indices[li];
    uint8_t mac[6];
    memcpy(mac, device_policies[p_idx].mac, 6);
    char d_name[DEVICE_NAME_BYTES] = "جهاز";
    int n = meter_find_name_locked(mac, false);
    if (n >= 0 && device_names[n].name[0]) {
      strncpy(d_name, device_names[n].name, sizeof(d_name) - 1);
    }
    bool is_conn = false;
    uint32_t d_ip = 0;
    for (size_t k = 0; k < MAX_CLIENTS; ++k) {
      if (memcmp(active_clients[k].mac, mac, 6) == 0 && active_clients[k].aid != 0) {
        is_conn = true;
        d_ip = active_clients[k].ip;
        break;
      }
    }
    bool blk = device_policies[p_idx].blocked;

    char lbuf[180];
    ip4_addr_t dip = {.addr = d_ip};
    if (is_conn) {
      snprintf(lbuf, sizeof(lbuf),
               "• <b>%s</b>: 🟢 متصل بالواي فاي (<code>%d.%d.%d.%d</code>) %s\n",
               d_name, IP2STR(&dip), blk ? "🔴 [محظور من النت]" : "✅ [الإنترنت نشط]");
    } else {
      snprintf(lbuf, sizeof(lbuf),
               "• <b>%s</b>: ⚪ غير متصل بالشبكة حالياً\n", d_name);
    }
    if (strlen(st_rep) + strlen(lbuf) < sizeof(st_rep) - 20) strcat(st_rep, lbuf);
  }
  portEXIT_CRITICAL(&data_mux);

  const char *kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 تحديث حالة الاتصال\",\"callback_data\":\"user_status\"}]]}";
  telegram_send_message_with_keyboard(chat_id, st_rep, kb);
}

static void send_admin_quota_report(const char *chat_id, const int *linked_indices, size_t linked_count) {
  uint64_t total_up = 0, total_down = 0;
  uint32_t live_up = 0, live_down = 0;
  uint16_t active_cnt = 0;

  portENTER_CRITICAL(&data_mux);
  for (size_t i = 0; i < usage_count; ++i) {
    total_up += usage_records[i].upload_bytes;
    total_down += usage_records[i].download_bytes;
  }
  live_up = total_cur_up_bps;
  live_down = total_cur_down_bps;
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
      active_cnt++;
    }
  }
  portEXIT_CRITICAL(&data_mux);

  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  double total_used_gb = (double)(total_up + total_down) / gb_div;
  double main_cap_gb = (double)main_quota_bytes / gb_div;
  double rem_gb = (main_cap_gb > total_used_gb) ? (main_cap_gb - total_used_gb) : 0.0;
  double used_pct = (main_cap_gb > 0) ? ((total_used_gb / main_cap_gb) * 100.0) : 0.0;

  time_t now = time(NULL);
  int32_t rem_days = -1, rem_hours = -1;
  if (now > 1700000000 && quota_end_date[0]) {
    time_t end_ts = meter_parse_date(quota_end_date, true);
    if (end_ts > now) {
      int64_t diff = (int64_t)(end_ts - now);
      rem_days = (int32_t)(diff / 86400);
      rem_hours = (int32_t)((diff % 86400) / 3600);
    } else {
      rem_days = 0; rem_hours = 0;
    }
  }

  char reply[1200];
  if (main_cap_gb > 0) {
    if (rem_days >= 0) {
      snprintf(reply, sizeof(reply),
               "📊 <b>تقرير باقة الإنترنت العامة (الراوتر):</b>\n\n"
               "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB (%.1f%%)\n"
               "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
               "📅 <b>تاريخ التجديد:</b> %s (متبقي %ld يوم و %ld ساعة)\n"
               "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل\n"
               "🚀 <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
               total_used_gb, main_cap_gb, used_pct, rem_gb,
               quota_end_date, (long)rem_days, (long)rem_hours, active_cnt,
               (double)live_down / 1000000.0, (double)live_up / 1000000.0);
    } else {
      snprintf(reply, sizeof(reply),
               "📊 <b>تقرير باقة الإنترنت العامة (الراوتر):</b>\n\n"
               "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB (%.1f%%)\n"
               "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
               "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل\n"
               "🚀 <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
               total_used_gb, main_cap_gb, used_pct, rem_gb,
               active_cnt,
               (double)live_down / 1000000.0, (double)live_up / 1000000.0);
    }
  } else {
    snprintf(reply, sizeof(reply),
             "📊 <b>إجمالي استهلاك راوتر ESP32:</b>\n\n"
             "📥 <b>إجمالي التحميل:</b> %.2f GB\n"
             "📤 <b>إجمالي الرفع:</b> %.2f GB\n"
             "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل\n"
             "🚀 <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
             (double)total_down / gb_div, (double)total_up / gb_div,
             active_cnt, (double)live_down / 1000000.0, (double)live_up / 1000000.0);
  }

  // If Admin has a personal linked device, append personal device section!
  if (linked_count > 0 && linked_indices) {
    for (size_t li = 0; li < linked_count && li < 2; ++li) {
      int p_idx = linked_indices[li];
      if (p_idx >= 0 && (size_t)p_idx < policy_count) {
        portENTER_CRITICAL(&data_mux);
        uint8_t mac[6];
        memcpy(mac, device_policies[p_idx].mac, 6);
        uint64_t my_quota = device_policies[p_idx].quota_bytes;
        char my_name[DEVICE_NAME_BYTES] = "هاتفك الشخصي";
        int n = meter_find_name_locked(mac, false);
        if (n >= 0 && device_names[n].name[0]) {
          strncpy(my_name, device_names[n].name, sizeof(my_name) - 1);
          my_name[sizeof(my_name) - 1] = 0;
        }
        int u = meter_find_usage_locked(mac, false);
        uint64_t my_used = (u >= 0) ? (usage_records[u].upload_bytes + usage_records[u].download_bytes) : 0;
        portEXIT_CRITICAL(&data_mux);

        double my_used_gb = (double)my_used / gb_div;
        char my_sec[250];
        if (my_quota > 0) {
          double my_q_gb = (double)my_quota / gb_div;
          double my_rem_gb = (my_q_gb > my_used_gb) ? (my_q_gb - my_used_gb) : 0.0;
          double my_pct = (my_used_gb * 100.0) / my_q_gb;
          snprintf(my_sec, sizeof(my_sec),
                   "\n📱 <b>باقة جهازك الشخصي (%s):</b>\n"
                   "   • المستهلك: %.2f GB من %.2f GB (%.1f%%)\n"
                   "   • المتبقي: %.2f GB\n",
                   my_name, my_used_gb, my_q_gb, my_pct, my_rem_gb);
        } else {
          snprintf(my_sec, sizeof(my_sec),
                   "\n📱 <b>استهلاك جهازك الشخصي (%s):</b>\n"
                   "   • المستهلك: %.2f GB (استهلاك مفتوح ♾️)\n",
                   my_name, my_used_gb);
        }
        if (strlen(reply) + strlen(my_sec) < sizeof(reply) - 80) {
          strcat(reply, my_sec);
        }
      }
    }
  }

  const char *kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 تحديث استهلاك الباقة\",\"callback_data\":\"adm_quota\"}]]}";
  telegram_send_message_with_keyboard(chat_id, reply, kb);
}

static void send_admin_speed_report(const char *chat_id, const int *linked_indices, size_t linked_count, const wifi_ap_record_t *ap_info) {
  uint32_t live_up = total_cur_up_bps;
  uint32_t live_down = total_cur_down_bps;
  uint16_t active_cnt = 0;

  portENTER_CRITICAL(&data_mux);
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
      active_cnt++;
    }
  }
  portEXIT_CRITICAL(&data_mux);

  char reply[800];
  snprintf(reply, sizeof(reply),
           "⚡ <b>فحص سرعة شبكة الراوتر اللحظية:</b>\n\n"
           "🚀 <b>السرعة الإجمالية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n"
           "👥 <b>الأجهزة المتصلة:</b> %u / 12 متصل\n"
           "📶 <b>الراوتر المزود:</b> %s (إشارة %d dBm)\n"
           "📡 <b>بث الواي فاي:</b> SoftAP Ch 1 (2.4GHz Wi-Fi 6)\n",
           (double)live_down / 1000000.0, (double)live_up / 1000000.0,
           active_cnt, ap_info ? (const char *)ap_info->ssid : "غير متصل",
           ap_info ? (int)ap_info->rssi : 0);

  // If Admin has a personal linked device, append its live speed!
  if (linked_count > 0 && linked_indices) {
    for (size_t li = 0; li < linked_count && li < 2; ++li) {
      int p_idx = linked_indices[li];
      if (p_idx >= 0 && (size_t)p_idx < policy_count) {
        portENTER_CRITICAL(&data_mux);
        uint8_t mac[6];
        memcpy(mac, device_policies[p_idx].mac, 6);
        char my_name[DEVICE_NAME_BYTES] = "هاتفك الشخصي";
        int n = meter_find_name_locked(mac, false);
        if (n >= 0 && device_names[n].name[0]) {
          strncpy(my_name, device_names[n].name, sizeof(my_name) - 1);
          my_name[sizeof(my_name) - 1] = 0;
        }
        int u = meter_find_usage_locked(mac, false);
        uint32_t my_down = (u >= 0) ? device_live_speeds[u].cur_down_bps : 0;
        uint32_t my_up = (u >= 0) ? device_live_speeds[u].cur_up_bps : 0;
        portEXIT_CRITICAL(&data_mux);

        char my_spd[220];
        snprintf(my_spd, sizeof(my_spd),
                 "\n📱 <b>سرعة جهازك الشخصي (%s):</b>\n"
                 "   ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
                 my_name, (double)my_down / 1000000.0, (double)my_up / 1000000.0);
        if (strlen(reply) + strlen(my_spd) < sizeof(reply) - 80) {
          strcat(reply, my_spd);
        }
      }
    }
  }

  const char *kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 فحص وتحديث السرعة\",\"callback_data\":\"adm_speed\"}]]}";
  telegram_send_message_with_keyboard(chat_id, reply, kb);
}

static void send_admin_devices_report(const char *chat_id) {
  char reply[1500] = "👥 <b>الأجهزة المتصلة والمسجلة بالراوتر:</b>\n\n";
  char kb_buttons[1200] = {0};
  uint16_t count = 0;
  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);

  uint8_t listed_macs[MAX_RECORDS][6];
  size_t listed_cnt = 0;

  portENTER_CRITICAL(&data_mux);
  // 1. Active connected clients first
  for (size_t k = 0; k < MAX_CLIENTS && listed_cnt < MAX_RECORDS; ++k) {
    if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
      memcpy(listed_macs[listed_cnt++], active_clients[k].mac, 6);
    }
  }
  // 2. Also include any blocked or quota-exceeded devices from device_policies so admin can top-up/unblock them even if offline
  for (size_t p = 0; p < policy_count && listed_cnt < MAX_RECORDS; ++p) {
    if (!valid_device_mac(device_policies[p].mac)) continue;
    int u_idx = meter_find_usage_locked(device_policies[p].mac, false);
    uint64_t used_b = (u_idx >= 0) ? (usage_records[u_idx].upload_bytes + usage_records[u_idx].download_bytes) : 0;
    bool quota_full = (device_policies[p].quota_bytes > 0 && used_b >= device_policies[p].quota_bytes);
    if (device_policies[p].blocked || quota_full || device_policies[p].quota_bytes > 0) {
      bool exists = false;
      for (size_t m = 0; m < listed_cnt; ++m) {
        if (memcmp(listed_macs[m], device_policies[p].mac, 6) == 0) { exists = true; break; }
      }
      if (!exists) {
        memcpy(listed_macs[listed_cnt++], device_policies[p].mac, 6);
      }
    }
  }

  for (size_t i = 0; i < listed_cnt; ++i) {
    uint8_t *mac = listed_macs[i];
    int p_idx = meter_find_policy_locked(mac, true);
    if (p_idx < 0) continue;

    bool is_online = false;
    uint32_t ip = 0;
    for (size_t k = 0; k < MAX_CLIENTS; ++k) {
      if (memcmp(active_clients[k].mac, mac, 6) == 0) {
        ip = active_clients[k].ip;
        is_online = (active_clients[k].aid != 0);
        break;
      }
    }
    if (ip == 0) {
      int b_idx = meter_find_binding_locked(mac, false);
      if (b_idx >= 0) ip = device_bindings[b_idx].bound_ip;
    }

    char name[DEVICE_NAME_BYTES] = "جهاز";
    int n_idx = meter_find_name_locked(mac, false);
    if (n_idx >= 0 && device_names[n_idx].name[0]) {
      strncpy(name, device_names[n_idx].name, sizeof(name) - 1);
      name[sizeof(name) - 1] = 0;
    } else if (ip != 0) {
      ip4_addr_t tip = {.addr = ip};
      snprintf(name, sizeof(name), "%d.%d.%d.%d", IP2STR(&tip));
    }

    int u_idx = meter_find_usage_locked(mac, false);
    uint32_t cur_down = (u_idx >= 0) ? device_live_speeds[u_idx].cur_down_bps : 0;
    uint32_t cur_up = (u_idx >= 0) ? device_live_speeds[u_idx].cur_up_bps : 0;
    uint64_t dev_used = (u_idx >= 0) ? (usage_records[u_idx].upload_bytes + usage_records[u_idx].download_bytes) : 0;
    uint64_t dev_quota = device_policies[p_idx].quota_bytes;
    bool is_blocked = device_policies[p_idx].blocked;
    bool is_vip = device_policies[p_idx].gaming_mode;

    count++;
    ip4_addr_t bip = {.addr = ip};
    char ip_s[24] = "--";
    if (ip != 0) snprintf(ip_s, sizeof(ip_s), "%d.%d.%d.%d", IP2STR(&bip));

    char q_info[64] = "";
    if (dev_quota > 0) {
      snprintf(q_info, sizeof(q_info), " / %.1f GB", (double)dev_quota / gb_div);
    }

    char dev_line[260];
    snprintf(dev_line, sizeof(dev_line),
             "%u. <b>%s</b> (<code>%s</code>) %s%s\n"
             "   ⬇️ %.2f M | ⬆️ %.2f M | 📊 %.2f%s GB\n",
             count, name, ip_s,
             is_blocked ? "🔴 [محظور]" : (is_online ? "🟢 [متصل]" : "⚪ [غير متصل]"),
             is_vip ? " 🎮 <b>VIP</b>" : "",
             (double)cur_down / 1000000.0, (double)cur_up / 1000000.0,
             (double)dev_used / gb_div, q_info);
    if (strlen(reply) + strlen(dev_line) < sizeof(reply) - 100) strcat(reply, dev_line);

    if (count <= 8) {
      char row[260];
      snprintf(row, sizeof(row),
               "%s[{\"text\":\"⚙️ تحكم وباقة وVIP: %s\",\"callback_data\":\"mdev:%d\"},"
               "{\"text\":\"%s\",\"callback_data\":\"%s:%d\"}]",
               kb_buttons[0] ? "," : "", name, p_idx,
               is_blocked ? "✅ فك حظر" : "🚫 حظر",
               is_blocked ? "ubp" : "blkp", p_idx);
      if (strlen(kb_buttons) + strlen(row) < sizeof(kb_buttons) - 90) strcat(kb_buttons, row);
    }
  }
  portEXIT_CRITICAL(&data_mux);

  if (count == 0) {
    strcat(reply, "<i>لا توجد أجهزة متصلة بالشبكة حالياً.</i>\n");
  } else {
    strcat(reply, "\n👇 <i>اضغط على (⚙️ تحكم وباقة وVIP) لأي جهاز لزيادة باقته أو تفعيل الـ VIP:</i>");
  }

  // Refresh button
  char refresh_btn[140];
  snprintf(refresh_btn, sizeof(refresh_btn), "%s[{\"text\":\"🔄 تحديث قائمة الأجهزة\",\"callback_data\":\"adm_devices\"}]",
           kb_buttons[0] ? "," : "");
  if (strlen(kb_buttons) + strlen(refresh_btn) < sizeof(kb_buttons) - 20) strcat(kb_buttons, refresh_btn);

  char kb_json[1350];
  snprintf(kb_json, sizeof(kb_json), "{\"inline_keyboard\":[%s]}", kb_buttons);
  telegram_send_message_with_keyboard(chat_id, reply, kb_json);
}

static void send_admin_device_control_menu(const char *chat_id, int p_idx) {
  if (p_idx < 0 || (size_t)p_idx >= policy_count) {
    telegram_send_message(chat_id, "⚠️ لم يتم العثور على بيانات هذا الجهاز.");
    return;
  }

  char rep[950];
  format_single_device_report(p_idx, rep, sizeof(rep));

  bool is_blk = false, is_vip = false;
  portENTER_CRITICAL(&data_mux);
  is_blk = device_policies[p_idx].blocked;
  is_vip = device_policies[p_idx].gaming_mode;
  portEXIT_CRITICAL(&data_mux);

  char kb[1050];
  snprintf(kb, sizeof(kb),
           "{\"inline_keyboard\":["
           "[{\"text\":\"➕ +1 GB\",\"callback_data\":\"addq:%d:1\"},"
           "{\"text\":\"➕ +5 GB\",\"callback_data\":\"addq:%d:5\"},"
           "{\"text\":\"➕ +10 GB\",\"callback_data\":\"addq:%d:10\"},"
           "{\"text\":\"➕ +20 GB\",\"callback_data\":\"addq:%d:20\"}],"
           "[{\"text\":\"♾️ باقة مفتوحة\",\"callback_data\":\"addq:%d:0\"},"
           "{\"text\":\"🔄 تصفير الاستهلاك\",\"callback_data\":\"rstu:%d\"}],"
           "[{\"text\":\"%s\",\"callback_data\":\"vip:%d\"},"
           "{\"text\":\"%s\",\"callback_data\":\"%s:%d\"}],"
           "[{\"text\":\"🔙 رجوع لقائمة الأجهزة\",\"callback_data\":\"adm_devices\"}]"
           "]}",
           p_idx, p_idx, p_idx, p_idx,
           p_idx, p_idx,
           is_vip ? "🎮 وضع VIP: مفعل ✅ (إلغاء)" : "🎮 تفعيل وضع الأولوية VIP 🚀", p_idx,
           is_blk ? "✅ إلغاء حظر الجهاز" : "🚫 حظر الجهاز من الإنترنت",
           is_blk ? "ubp" : "blkp", p_idx);

  telegram_send_message_with_keyboard(chat_id, rep, kb);
}

static void send_admin_reboot_menu(const char *chat_id) {
  char sched_str[120];
  if (auto_reboot_enabled) {
    int h12 = auto_reboot_hour % 12;
    if (h12 == 0) h12 = 12;
    const char *ampm = (auto_reboot_hour < 12) ? "صباحاً (ص)" : "مساءً (م)";
    snprintf(sched_str, sizeof(sched_str), "🟢 مفعل يومياً الساعة <b>%d:%02d %s</b>", h12, auto_reboot_min, ampm);
  } else {
    snprintf(sched_str, sizeof(sched_str), "⚪ غير مفعل حالياً");
  }

  char msg[750];
  snprintf(msg, sizeof(msg),
           "🔄 <b>إدارة إعادة تشغيل الراوتر والجدولة اليومية:</b>\n\n"
           "⏰ <b>الموعد المجدول حالياً:</b> %s\n\n"
           "👇 اختر من القائمة بالأسفل <b>إعادة التشغيل الفوري</b> أو ضبط <b>موعد إعادة تشغيل يومي تلقائي</b> لتنظيف الذاكرة:\n"
           "💡 <i>لتحديد أي وقت مخصص، أرسل مثلاً:</i> <code>/reboot_time 04:30</code>",
           sched_str);

  char kb[1050];
  snprintf(kb, sizeof(kb),
           "{\"inline_keyboard\":["
           "[{\"text\":\"🔴 إعادة تشغيل الراوتر الآن فوراً\",\"callback_data\":\"reboot_confirm\"}],"
           "[{\"text\":\"%s🕐 1:00 ص\",\"callback_data\":\"rbs:1\"},"
           "{\"text\":\"%s🕓 4:00 ص\",\"callback_data\":\"rbs:4\"},"
           "{\"text\":\"%s🕕 6:00 ص\",\"callback_data\":\"rbs:6\"}],"
           "[{\"text\":\"%s🕐 1:00 م\",\"callback_data\":\"rbs:13\"},"
           "{\"text\":\"%s🕕 6:00 م\",\"callback_data\":\"rbs:18\"},"
           "{\"text\":\"%s🕘 9:00 م\",\"callback_data\":\"rbs:21\"}],"
           "[{\"text\":\"❌ إلغاء جدولة إعادة التشغيل التلقائي\",\"callback_data\":\"rbs:off\"}]"
           "]}",
           (auto_reboot_enabled && auto_reboot_hour == 1 && auto_reboot_min == 0) ? "✅ " : "",
           (auto_reboot_enabled && auto_reboot_hour == 4 && auto_reboot_min == 0) ? "✅ " : "",
           (auto_reboot_enabled && auto_reboot_hour == 6 && auto_reboot_min == 0) ? "✅ " : "",
           (auto_reboot_enabled && auto_reboot_hour == 13 && auto_reboot_min == 0) ? "✅ " : "",
           (auto_reboot_enabled && auto_reboot_hour == 18 && auto_reboot_min == 0) ? "✅ " : "",
           (auto_reboot_enabled && auto_reboot_hour == 21 && auto_reboot_min == 0) ? "✅ " : "");

  telegram_send_message_with_keyboard(chat_id, msg, kb);
}

static void send_admin_status_report(const char *chat_id, const wifi_ap_record_t *ap_info) {
  uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  uint32_t days = uptime_s / 86400;
  uint32_t hours = (uptime_s % 86400) / 3600;
  uint32_t mins = (uptime_s % 3600) / 60;
  uint32_t free_heap_kb = esp_get_free_heap_size() / 1024;

  uint16_t active_cnt = 0;
  portENTER_CRITICAL(&data_mux);
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
      active_cnt++;
    }
  }
  portEXIT_CRITICAL(&data_mux);

  char sched_str[80] = "غير مفعل";
  if (auto_reboot_enabled) {
    int h12 = auto_reboot_hour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(sched_str, sizeof(sched_str), "%d:%02d %s يومياً", h12, auto_reboot_min, (auto_reboot_hour < 12) ? "ص" : "م");
  }

  char reply[850];
  snprintf(reply, sizeof(reply),
           "⚙️ <b>حالة نظام راوتر ESP32 والتحكم بالمعالج:</b>\n\n"
           "⏱️ <b>مدة التشغيل:</b> %lu يوم و %lu ساعة و %lu دقيقة\n"
           "🧠 <b>الذاكرة الحرة (RAM):</b> %lu KB\n"
           "🚀 <b>تردد المعالج (CPU):</b> %u MHz\n"
           "⏰ <b>الريستارت التلقائي:</b> %s\n"
           "📶 <b>الراوتر المزود:</b> %s (إشارة %d dBm)\n"
           "👥 <b>الأجهزة المتصلة:</b> %u / 12 متصل\n"
           "⚡ <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n\n"
           "👇 <i>يمكنك تغيير تردد المعالج مباشرة من الأزرار بالأسفل:</i>",
           (unsigned long)days, (unsigned long)hours, (unsigned long)mins,
           (unsigned long)free_heap_kb, (unsigned)esp_cpu_freq_mhz,
           sched_str,
           ap_info ? (const char *)ap_info->ssid : "غير متصل",
           ap_info ? (int)ap_info->rssi : 0, active_cnt,
           (double)total_cur_down_bps / 1000000.0, (double)total_cur_up_bps / 1000000.0);

  char kb[550];
  snprintf(kb, sizeof(kb),
           "{\"inline_keyboard\":["
           "[{\"text\":\"%s🔋 80 MHz (توفير)\",\"callback_data\":\"cpu:80\"},"
           "{\"text\":\"%s⚡ 160 MHz (متوازن)\",\"callback_data\":\"cpu:160\"},"
           "{\"text\":\"%s🚀 240 MHz (أقصى سرعة)\",\"callback_data\":\"cpu:240\"}],"
           "[{\"text\":\"🔄 تحديث حالة الراوتر\",\"callback_data\":\"adm_status\"}]"
           "]}",
           (esp_cpu_freq_mhz == 80) ? "✅ " : "",
           (esp_cpu_freq_mhz == 160) ? "✅ " : "",
           (esp_cpu_freq_mhz == 240) ? "✅ " : "");
  telegram_send_message_with_keyboard(chat_id, reply, kb);
}

static void telegram_ack_offset_before_reboot(void) {
  if (!telegram_token[0] || last_update_id <= 0) return;
  if (!tg_http_lock(pdMS_TO_TICKS(5000))) return;

  char url[220];
  snprintf(url, sizeof(url),
           "https://api.telegram.org/bot%s/getUpdates?offset=%" PRIi64 "&timeout=0&limit=1",
           telegram_token, last_update_id + 1);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 5000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 512,
      .buffer_size_tx = 256,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client) {
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
  }
  tg_http_unlock();
}

// -----------------------------------------------------------------------------
// Interactive Telegram Bot Command Handler (Background Long Polling FreeRTOS Task)
// -----------------------------------------------------------------------------
static void telegram_task(void *pvParameters) {
  (void)pvParameters;
  ESP_LOGI(TG_TAG, "Telegram polling task started on Core %d", xPortGetCoreID());

  char *rx_buffer = (char *)malloc(4096);
  if (!rx_buffer) {
    ESP_LOGE(TG_TAG, "Failed to allocate memory for Telegram rx buffer");
    vTaskDelete(NULL);
    return;
  }

  while (true) {
    // Wait for Wi-Fi STA connection
    wifi_ap_record_t ap_info;
        // Post-Boot Alert for Daily Scheduled Auto-Reboot
    if (config_get_scheduled_reboot_flag()) {
      config_set_scheduled_reboot_flag(false);
      if (telegram_enabled && telegram_token[0] && telegram_chat_id[0]) {
        char reboot_msg[512];
        uint32_t free_ram_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
        snprintf(reboot_msg, sizeof(reboot_msg),
                 "🔄 <b>تمت إعادة التشغيل اليومية المجدولة بنجاح!</b>\n\n"
                 "✅ تم تنظيف الذاكرة ومسح الـ Cache وحفظ العدادات.\n"
                 "🧠 <b>الذاكرة الحرة (Free RAM):</b> %lu KB\n"
                 "⏱️ <b>الموعد المجدول:</b> %02d:%02d يومياً (UTC%+d)\n"
                 "🚀 راوتر ESP32 يعمل الآن بأقصى سرعة واستقرار.",
                 (unsigned long)free_ram_kb,
                 auto_reboot_hour, auto_reboot_min, auto_reboot_tz_offset);
        telegram_broadcast_admins(reboot_msg);
      }
    }

    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK || !telegram_token[0]) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    // Check automated quota alerts safely within telegram_task
    telegram_check_quota_alerts();

    // Anti-Congestion Priority: If user is actively browsing/streaming/testing speed (>3 Mbps),
    // yield polling so packet forwarding achieves wire-speed throughput without TLS contention or latency!
    uint32_t traffic_bps = total_cur_down_bps + total_cur_up_bps;
    if (traffic_bps > 3000000) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    char url[220];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/getUpdates?offset=%" PRIi64 "&timeout=2&limit=5",
             telegram_token, last_update_id + 1);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };

    if (!tg_http_lock(pdMS_TO_TICKS(4000))) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    int total_read = 0;
    if (client) {
      esp_err_t err = esp_http_client_open(client, 0);
      if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        while (total_read < 4090) {
          int r = esp_http_client_read(client, rx_buffer + total_read, 4090 - total_read);
          if (r <= 0) break;
          total_read += r;
        }
        rx_buffer[total_read] = 0;
      }
      esp_http_client_cleanup(client);
    }
    tg_http_unlock();

    if (total_read > 0) {
        char *ptr = rx_buffer;
        while ((ptr = strstr(ptr, "\"update_id\":")) != NULL) {
          ptr += 12;
          int64_t up_id = strtoll(ptr, NULL, 10);
          if (up_id > last_update_id) {
            last_update_id = up_id;
          }

          char *next_up = strstr(ptr, "\"update_id\":");

          // -------------------------------------------------------------------
          // A. Check for Telegram Callback Query (Inline Keyboard Button Tap)
          // -------------------------------------------------------------------
          char *cb_ptr = strstr(ptr, "\"callback_query\":");
          if (cb_ptr && (!next_up || cb_ptr < next_up)) {
            char cb_id[64] = {0};
            char cb_sender[32] = {0};
            char cb_data[64] = {0};

            // Extract callback id
            char *id_p = strstr(cb_ptr, "\"id\":");
            if (id_p && id_p < cb_ptr + 80) {
              id_p += 5;
              while (*id_p == ' ' || *id_p == '"') id_p++;
              size_t c_idx = 0;
              while (*id_p && *id_p != '"' && c_idx < sizeof(cb_id) - 1) {
                cb_id[c_idx++] = *id_p++;
              }
              cb_id[c_idx] = 0;
            }

            // Extract sender from.id
            char *from_p = strstr(cb_ptr, "\"from\":");
            if (from_p && from_p < cb_ptr + 150) {
              char *from_id_p = strstr(from_p, "\"id\":");
              if (from_id_p && from_id_p < from_p + 100) {
                from_id_p += 5;
                while (*from_id_p == ' ') from_id_p++;
                size_t s_idx = 0;
                while ((*from_id_p == '-' || (*from_id_p >= '0' && *from_id_p <= '9')) && s_idx < sizeof(cb_sender) - 1) {
                  cb_sender[s_idx++] = *from_id_p++;
                }
                cb_sender[s_idx] = 0;
              }
            }

            // Extract callback data
            char *data_p = strstr(cb_ptr, "\"data\":");
            if (data_p && (!next_up || data_p < next_up)) {
              data_p += 7;
              while (*data_p == ' ') data_p++;
              if (*data_p == '"') data_p++;
              json_decode_string(data_p, cb_data, sizeof(cb_data));
            }

            if (cb_sender[0] && cb_data[0]) {
              ESP_LOGI(TG_TAG, "Callback query received: data='%s' from chat=%s", cb_data, cb_sender);

              // Gather linked devices for this callback sender
              int cb_linked_indices[MAX_RECORDS];
              size_t cb_linked_count = 0;
              portENTER_CRITICAL(&data_mux);
              for (size_t p = 0; p < policy_count; ++p) {
                if (device_policies[p].telegram_chat_id[0] != 0 &&
                    strcmp(device_policies[p].telegram_chat_id, cb_sender) == 0) {
                  if (cb_linked_count < MAX_RECORDS) cb_linked_indices[cb_linked_count++] = (int)p;
                }
              }
              portEXIT_CRITICAL(&data_mux);
              bool is_cb_admin = telegram_is_admin(cb_sender);
              bool answered_toast = false;

              if (strcmp(cb_data, "user_quota") == 0) {
                send_user_quota_report(cb_sender, cb_linked_indices, cb_linked_count);
              } else if (strcmp(cb_data, "user_speed") == 0) {
                send_user_speed_report(cb_sender, cb_linked_indices, cb_linked_count);
              } else if (strcmp(cb_data, "user_status") == 0) {
                send_user_status_report(cb_sender, cb_linked_indices, cb_linked_count);
              } else if (strcmp(cb_data, "adm_quota") == 0 && is_cb_admin) {
                send_admin_quota_report(cb_sender, cb_linked_indices, cb_linked_count);
              } else if (strcmp(cb_data, "adm_speed") == 0 && is_cb_admin) {
                send_admin_speed_report(cb_sender, cb_linked_indices, cb_linked_count, &ap_info);
              } else if (strcmp(cb_data, "adm_devices") == 0 && is_cb_admin) {
                send_admin_devices_report(cb_sender);
              } else if (strcmp(cb_data, "adm_status") == 0 && is_cb_admin) {
                send_admin_status_report(cb_sender, &ap_info);
              } else if (strcmp(cb_data, "adm_reboot") == 0 && is_cb_admin) {
                send_admin_reboot_menu(cb_sender);
              } else if (strncmp(cb_data, "mdev:", 5) == 0 && is_cb_admin) {
                int p_idx = atoi(cb_data + 5);
                send_admin_device_control_menu(cb_sender, p_idx);
              } else if (strncmp(cb_data, "addq:", 5) == 0 && is_cb_admin) {
                int p_idx = -1, add_gb = 0;
                if (sscanf(cb_data + 5, "%d:%d", &p_idx, &add_gb) == 2 && p_idx >= 0 && (size_t)p_idx < policy_count) {
                  uint8_t mac[6];
                  portENTER_CRITICAL(&data_mux);
                  memcpy(mac, device_policies[p_idx].mac, 6);
                  portEXIT_CRITICAL(&data_mux);
                  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
                  int64_t add_bytes = (add_gb > 0) ? (int64_t)((double)add_gb * gb_div) : 0;
                  meter_add_device_quota(mac, add_bytes);
                  if (cb_id[0]) {
                    char toast[96];
                    if (add_gb > 0) snprintf(toast, sizeof(toast), "✅ تمت إضافة +%d GB وفك الحظر عن الجهاز!", add_gb);
                    else snprintf(toast, sizeof(toast), "♾️ تم فتح الباقة بلا حدود وفك الحظر!");
                    telegram_answer_callback_query(cb_id, toast);
                    answered_toast = true;
                  }
                  send_admin_device_control_menu(cb_sender, p_idx);
                }
              } else if (strncmp(cb_data, "rstu:", 5) == 0 && is_cb_admin) {
                int p_idx = atoi(cb_data + 5);
                if (p_idx >= 0 && (size_t)p_idx < policy_count) {
                  uint8_t mac[6];
                  portENTER_CRITICAL(&data_mux);
                  memcpy(mac, device_policies[p_idx].mac, 6);
                  portEXIT_CRITICAL(&data_mux);
                  meter_reset_device_usage(mac);
                  if (cb_id[0]) {
                    telegram_answer_callback_query(cb_id, "🔄 تم تصفير استهلاك الجهاز وفك الحظر!");
                    answered_toast = true;
                  }
                  send_admin_device_control_menu(cb_sender, p_idx);
                }
              } else if (strncmp(cb_data, "vip:", 4) == 0 && is_cb_admin) {
                int p_idx = atoi(cb_data + 4);
                if (p_idx >= 0 && (size_t)p_idx < policy_count) {
                  uint8_t mac[6];
                  bool cur_vip = false;
                  portENTER_CRITICAL(&data_mux);
                  memcpy(mac, device_policies[p_idx].mac, 6);
                  cur_vip = device_policies[p_idx].gaming_mode;
                  portEXIT_CRITICAL(&data_mux);
                  meter_set_device_gaming(mac, !cur_vip);
                  if (cb_id[0]) {
                    telegram_answer_callback_query(cb_id, !cur_vip ? "🎮 تم تفعيل وضع الأولوية VIP للجهاز!" : "⚪ تم إلغاء وضع VIP للجهاز.");
                    answered_toast = true;
                  }
                  send_admin_device_control_menu(cb_sender, p_idx);
                }
              } else if (strncmp(cb_data, "blkp:", 5) == 0 && is_cb_admin) {
                int p_idx = atoi(cb_data + 5);
                if (p_idx >= 0 && (size_t)p_idx < policy_count) {
                  uint8_t mac[6];
                  portENTER_CRITICAL(&data_mux);
                  memcpy(mac, device_policies[p_idx].mac, 6);
                  portEXIT_CRITICAL(&data_mux);
                  meter_set_device_blocked(mac, true);
                  if (cb_id[0]) {
                    telegram_answer_callback_query(cb_id, "🚫 تم حظر الجهاز من الإنترنت!");
                    answered_toast = true;
                  }
                  send_admin_device_control_menu(cb_sender, p_idx);
                }
              } else if (strncmp(cb_data, "ubp:", 4) == 0 && is_cb_admin) {
                int p_idx = atoi(cb_data + 4);
                if (p_idx >= 0 && (size_t)p_idx < policy_count) {
                  uint8_t mac[6];
                  portENTER_CRITICAL(&data_mux);
                  memcpy(mac, device_policies[p_idx].mac, 6);
                  portEXIT_CRITICAL(&data_mux);
                  meter_set_device_blocked(mac, false);
                  if (cb_id[0]) {
                    telegram_answer_callback_query(cb_id, "✅ تم فك حظر الجهاز بنجاح!");
                    answered_toast = true;
                  }
                  send_admin_device_control_menu(cb_sender, p_idx);
                }
              } else if (strncmp(cb_data, "cpu:", 4) == 0 && is_cb_admin) {
                int freq = atoi(cb_data + 4);
                if (freq == 80 || freq == 160 || freq == 240) {
                  config_save_performance((uint16_t)freq, esp_wifi_power_save);
                  if (cb_id[0]) {
                    char toast[80];
                    snprintf(toast, sizeof(toast), "🚀 تم ضبط تردد المعالج على %d MHz وحفظه!", freq);
                    telegram_answer_callback_query(cb_id, toast);
                    answered_toast = true;
                  }
                  send_admin_status_report(cb_sender, &ap_info);
                }
              } else if (strncmp(cb_data, "rbs:", 4) == 0 && is_cb_admin) {
                const char *arg = cb_data + 4;
                if (strcmp(arg, "off") == 0) {
                  config_save_auto_reboot(false, auto_reboot_hour, auto_reboot_min, auto_reboot_tz_offset);
                  meter_reset_reboot_fired_state();
                  if (cb_id[0]) {
                    telegram_answer_callback_query(cb_id, "❌ تم إلغاء جدولة إعادة التشغيل اليومية.");
                    answered_toast = true;
                  }
                } else {
                  int hr = atoi(arg);
                  if (hr >= 0 && hr < 24) {
                    config_save_auto_reboot(true, (uint8_t)hr, 0, auto_reboot_tz_offset);
                    meter_reset_reboot_fired_state();
                    if (cb_id[0]) {
                      int h12 = hr % 12;
                      if (h12 == 0) h12 = 12;
                      char toast[96];
                      snprintf(toast, sizeof(toast), "⏰ تم تفعيل إعادة التشغيل يومياً الساعة %d:00 %s!", h12, (hr < 12) ? "صباحاً" : "مساءً");
                      telegram_answer_callback_query(cb_id, toast);
                      answered_toast = true;
                    }
                  }
                }
                send_admin_reboot_menu(cb_sender);
              } else if (strncmp(cb_data, "blk:", 4) == 0 && is_cb_admin) {
                const char *tip = cb_data + 4;
                uint8_t t_mac[6] = {0};
                char t_name[DEVICE_NAME_BYTES] = {0};
                uint32_t t_ip = 0;
                if (find_device_by_target(tip, t_mac, t_name, sizeof(t_name), &t_ip)) {
                  meter_set_device_blocked(t_mac, true);
                  if (cb_id[0]) {
                    telegram_answer_callback_query(cb_id, "🚫 تم حظر الجهاز بنجاح!");
                    answered_toast = true;
                  }
                  send_admin_devices_report(cb_sender);
                }
              } else if (strncmp(cb_data, "unblk:", 6) == 0 && is_cb_admin) {
                const char *tip = cb_data + 6;
                uint8_t t_mac[6] = {0};
                char t_name[DEVICE_NAME_BYTES] = {0};
                uint32_t t_ip = 0;
                if (find_device_by_target(tip, t_mac, t_name, sizeof(t_name), &t_ip)) {
                  meter_set_device_blocked(t_mac, false);
                  if (cb_id[0]) {
                    telegram_answer_callback_query(cb_id, "✅ تم إلغاء حظر الجهاز!");
                    answered_toast = true;
                  }
                  send_admin_devices_report(cb_sender);
                }
              } else if (strcmp(cb_data, "reboot_confirm") == 0 && is_cb_admin) {
                if (cb_id[0]) {
                  telegram_answer_callback_query(cb_id, "🔄 جاري إعادة التشغيل...");
                  answered_toast = true;
                }
                // Acknowledge update offset with Telegram BEFORE rebooting so Telegram never re-sends reboot_confirm on next boot!
                telegram_ack_offset_before_reboot();
                telegram_send_message_with_keyboard(cb_sender, "🔄 <b>جاري حفظ العدادات وإعادة تشغيل الراوتر الآن بأمان...</b>", TG_ADMIN_KEYBOARD);
                meter_save_usage();
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
              } else if (strncmp(cb_data, "dev_idx:", 8) == 0) {
                int dev_i = atoi(cb_data + 8);
                char rep[900];
                format_single_device_report(dev_i, rep, sizeof(rep));
                telegram_send_message(cb_sender, rep);
              } else if (strcmp(cb_data, "dev_all") == 0) {
                char sum_rep[1000], kb_rep[600];
                format_multi_device_summary(cb_linked_indices, cb_linked_count, sum_rep, sizeof(sum_rep), kb_rep, sizeof(kb_rep));
                telegram_send_message_with_keyboard(cb_sender, sum_rep, kb_rep);
              }

              if (cb_id[0] && !answered_toast) {
                telegram_answer_callback_query(cb_id, NULL);
              }
            } else if (cb_id[0]) {
              telegram_answer_callback_query(cb_id, NULL);
            }
          }

          // -------------------------------------------------------------------
          // B. Check for Telegram Standard Message
          // -------------------------------------------------------------------
          char *msg_ptr = strstr(ptr, "\"message\":");
          if (msg_ptr && (!next_up || msg_ptr < next_up)) {
            // Locate sender chat ID
            char *chat_ptr = strstr(msg_ptr, "\"chat\":");
            char sender_chat_id[32] = {0};
            if (chat_ptr) {
              char *id_ptr = strstr(chat_ptr, "\"id\":");
              if (id_ptr && id_ptr < chat_ptr + 100) {
                id_ptr += 5;
                size_t k = 0;
                while ((*id_ptr == ' ' || *id_ptr == '-' || (*id_ptr >= '0' && *id_ptr <= '9')) && k < sizeof(sender_chat_id) - 1) {
                  if (*id_ptr != ' ') sender_chat_id[k++] = *id_ptr;
                  id_ptr++;
                }
                sender_chat_id[k] = 0;
              }
            }

            // Extract sender's first_name and username from "from":{...}
            char sender_first_name[48] = {0};
            char sender_username[32] = {0};
            char *from_ptr = strstr(msg_ptr, "\"from\":");
            if (from_ptr && (!next_up || from_ptr < next_up)) {
              char *fn_ptr = strstr(from_ptr, "\"first_name\":");
              if (fn_ptr && fn_ptr < from_ptr + 250) {
                fn_ptr += 13;
                while (*fn_ptr == ' ') fn_ptr++;
                if (*fn_ptr == '"') fn_ptr++;
                json_decode_string(fn_ptr, sender_first_name, sizeof(sender_first_name));
              }
              char *un_ptr = strstr(from_ptr, "\"username\":");
              if (un_ptr && un_ptr < from_ptr + 250) {
                un_ptr += 11;
                while (*un_ptr == ' ') un_ptr++;
                if (*un_ptr == '"') un_ptr++;
                json_decode_string(un_ptr, sender_username, sizeof(sender_username));
              }
            }

            // Locate message text
            char *text_ptr = strstr(msg_ptr, "\"text\":");
            char cmd_text[256] = {0};
            if (text_ptr && (!next_up || text_ptr < next_up)) {
              text_ptr += 7;
              while (*text_ptr == ' ') text_ptr++;
              if (*text_ptr == '"') text_ptr++;
              json_decode_string(text_ptr, cmd_text, sizeof(cmd_text));
            }

            if (!sender_chat_id[0] || !cmd_text[0]) {
              continue;
            }

            // Auto-register discovered user
            telegram_register_discovered_user(sender_chat_id, sender_first_name, sender_username);

            // Check if sender is admin or first-time setup
            bool is_admin = false;
            if (telegram_chat_id[0] == 0) {
              // First time setup: auto-bind chat ID as administrator
              snprintf(telegram_chat_id, sizeof(telegram_chat_id), "%s", sender_chat_id);
              config_save_telegram(telegram_enabled, telegram_interactive, telegram_token, telegram_chat_id, telegram_thresholds);
              is_admin = true;
            } else {
              is_admin = telegram_is_admin(sender_chat_id);
            }

            // Check for devices linked to this sender's chat_id
            int linked_indices[MAX_RECORDS];
            size_t linked_count = 0;
            portENTER_CRITICAL(&data_mux);
            for (size_t p = 0; p < policy_count; ++p) {
              if (device_policies[p].telegram_chat_id[0] != 0 &&
                  strcmp(device_policies[p].telegram_chat_id, sender_chat_id) == 0) {
                if (linked_count < MAX_RECORDS) {
                  linked_indices[linked_count++] = (int)p;
                }
              }
            }
            portEXIT_CRITICAL(&data_mux);

            ESP_LOGI(TG_TAG, "Processing message '%s' from %s (name='%s', is_admin=%d, linked_devices=%u)",
                     cmd_text, sender_chat_id, sender_first_name, is_admin, (unsigned)linked_count);

            // -----------------------------------------------------------------
            // Dynamic IP Self-Linking Check (e.g. /link 192.168.4.2 or 192.168.4.2)
            // -----------------------------------------------------------------
            char potential_ip[256] = {0};
            bool has_link_cmd = false;
            if (strstr(cmd_text, "ربط جهازي") != NULL || strstr(cmd_text, "🔗") != NULL) {
              char ask_ip[500];
              snprintf(ask_ip, sizeof(ask_ip),
                       "🔗 <b>ربط جهازك بالبوت لمتابعة الاستهلاك:</b>\n\n"
                       "أرسل عنوان الـ IP لجهازك المتصل حالياً بالراوتر، مثل:\n"
                       "<code>192.168.4.2</code> أو <code>/link 192.168.4.2</code>\n\n"
                       "💡 تجد عنوان IP في تفاصيل شبكة الواي فاي بهاتفك أو كمبيوترك.\n"
                       "🆔 معرفك: <code>%s</code>",
                       sender_chat_id);
              telegram_send_message_with_keyboard(sender_chat_id, ask_ip, TG_UNLINKED_KEYBOARD);
              continue;
            } else if (strncmp(cmd_text, "/link", 5) == 0) {
              has_link_cmd = true;
              const char *p = cmd_text + 5;
              while (*p == ' ') p++;
              snprintf(potential_ip, sizeof(potential_ip), "%s", p);
            } else if (strncmp(cmd_text, "ربط", 6) == 0 || strncmp(cmd_text, "ربط", 3) == 0) {
              has_link_cmd = true;
              const char *p = strstr(cmd_text, "ربط") + strlen("ربط");
              while (*p == ' ') p++;
              snprintf(potential_ip, sizeof(potential_ip), "%s", p);
            } else if (!is_admin) {
              // If not admin, check if the whole text is an IP address
              snprintf(potential_ip, sizeof(potential_ip), "%s", cmd_text);
            }

            char *tip = potential_ip;
            while (*tip == ' ') tip++;
            char *tend = tip + strlen(tip) - 1;
            while (tend > tip && (*tend == ' ' || *tend == '\r' || *tend == '\n')) *tend-- = '\0';

            ip4_addr_t check_ip;
            if (tip[0] && ip4addr_aton(tip, &check_ip)) {
              uint8_t t_mac[6] = {0};
              char t_name[DEVICE_NAME_BYTES] = {0};
              uint32_t t_ip = 0;
              if (find_device_by_target(tip, t_mac, t_name, sizeof(t_name), &t_ip)) {
                meter_set_device_telegram(t_mac, sender_chat_id);

                char confirm_msg[600];
                snprintf(confirm_msg, sizeof(confirm_msg),
                         "🎉 <b>تم ربط جهازك بنجاح!</b> ✅\n\n"
                         "📱 <b>الجهاز:</b> %s\n"
                         "🌐 <b>عنوان IP:</b> %s\n"
                         "🆔 <b>معرف التيليجرام:</b> <code>%s</code>\n\n"
                         "يمكنك الآن متابعة استهلاك باقتك وسرعة اتصالك وفحص حالتك مباشرة من الأزرار بالأسفل! 🚀",
                         t_name, tip, sender_chat_id);
                telegram_send_message_with_keyboard(sender_chat_id, confirm_msg, TG_USER_KEYBOARD);

                // Notify all administrators
                char adm_notif[450];
                snprintf(adm_notif, sizeof(adm_notif),
                         "🔔 <b>تم ربط جهاز جديد عبر التيليجرام:</b>\n\n"
                         "👤 <b>المستخدم:</b> %s%s%s\n"
                         "📱 <b>الجهاز:</b> %s (<code>%s</code>)\n"
                         "🆔 <b>Chat ID:</b> <code>%s</code>",
                         sender_first_name[0] ? sender_first_name : "مستخدم",
                         sender_username[0] ? " (@" : "",
                         sender_username[0] ? sender_username : (sender_username[0] ? ")" : ""),
                         t_name, tip, sender_chat_id);
                telegram_broadcast_admins(adm_notif);
                continue;
              } else {
                char not_found[450];
                snprintf(not_found, sizeof(not_found),
                         "⚠️ <b>لم يتم العثور على جهاز متصل بعنوان IP:</b> <code>%s</code>\n\n"
                         "تأكد أن جهازك متصل بشبكة واي فاي الراوتر حالياً ومسجل بالشبكة، ثم أعد المحاولة.",
                         tip);
                telegram_send_message_with_keyboard(sender_chat_id, not_found,
                                                    linked_count > 0 ? TG_USER_KEYBOARD : TG_UNLINKED_KEYBOARD);
                continue;
              }
            } else if (has_link_cmd) {
              char inv_ip[450];
              snprintf(inv_ip, sizeof(inv_ip),
                       "⚠️ <b>عنوان IP غير صالح:</b> '%.32s'\n\n"
                       "يرجى كتابة عنوان IP صحيح، مثال:\n"
                       "<code>/link 192.168.4.2</code>", tip);
              telegram_send_message_with_keyboard(sender_chat_id, inv_ip,
                                                  linked_count > 0 ? TG_USER_KEYBOARD : TG_UNLINKED_KEYBOARD);
              continue;
            }

            // =================================================================
            // CASE 1: UNRECOGNIZED SENDER (Not Admin & No Linked Devices)
            // =================================================================
            if (!is_admin && linked_count == 0) {
              char polite_reply[600];
              snprintf(polite_reply, sizeof(polite_reply),
                       "👋 <b>مرحباً بك في بوت راوتر ESP32 الذكي!</b> 🌐\n\n"
                       "🆔 <b>معرف المحادثة الخاص بك (Chat ID):</b>\n"
                       "<code>%s</code>\n\n"
                       "⚠️ <i>هذا الحساب غير مربوط بأي جهاز في الشبكة حالياً.</i>\n\n"
                       "🔗 <b>لربط جهازك تلقائياً:</b>\n"
                       "أرسل عنوان IP لجهازك الآن (مثال: <code>192.168.4.2</code>) أو اضغط <b>ربط جهازي</b> بالأسفل، أو زود مدير الشبكة بمعرفك لربطه من لوحة التحكم.",
                       sender_chat_id);
              telegram_send_message_with_keyboard(sender_chat_id, polite_reply, TG_UNLINKED_KEYBOARD);
              continue;
            }

            // =================================================================
            // CASE 2: REGULAR LINKED USER (Not Admin)
            // =================================================================
            if (!is_admin && linked_count > 0) {
              // 1. Quota
              if (strstr(cmd_text, "/quota") || strstr(cmd_text, "استهلاك") || strstr(cmd_text, "باقة") || strstr(cmd_text, "باقتي") || strstr(cmd_text, "كوتا")) {
                send_user_quota_report(sender_chat_id, linked_indices, linked_count);
              }
              // 2. Speed
              else if (strstr(cmd_text, "/speed") || strstr(cmd_text, "سرعة") || strstr(cmd_text, "سرعتي") || strstr(cmd_text, "السرعة")) {
                send_user_speed_report(sender_chat_id, linked_indices, linked_count);
              }
              // 3. Status
              else if (strstr(cmd_text, "/status") || strstr(cmd_text, "اتصالي") || strstr(cmd_text, "فحص") || strstr(cmd_text, "حالة")) {
                send_user_status_report(sender_chat_id, linked_indices, linked_count);
              }
              // 4. User Help / Default
              else {
                char u_help[700];
                snprintf(u_help, sizeof(u_help),
                         "👋 <b>مرحباً بك! أجهزتك مربوطة بهذا الحساب بنجاح 🌐</b>\n\n"
                         "استخدم الأزرار التفاعلية بالأسفل لمتابعة حسابك:\n"
                         "📊 <b>استهلاك باقتي</b>: متابعة كمية الجيجابايت المستهلكة والمتبقية\n"
                         "⚡ <b>سرعة اتصالي</b>: فحص سرعة التحميل والرفع اللحظية\n"
                         "🌐 <b>فحص اتصالي</b>: التحقق من تواجد جهازك على الشبكة\n"
                         "📋 <b>مساعدة</b>: عرض هذا الدليل الإرشادي\n\n"
                         "💡 <i>لربط جهاز إضافي، أرسل عنوان الـ IP الخاص به في أي وقت.</i>");
                telegram_send_message_with_keyboard(sender_chat_id, u_help, TG_USER_KEYBOARD);
              }
              continue;
            }

            // =================================================================
            // CASE 3: ADMINISTRATOR (Full Router Management Capabilities)
            // =================================================================
            if (is_admin) {
              // 1. Command: /quota, استهلاك, باقة, كوتا, الباقة
              if (strstr(cmd_text, "/quota") || strstr(cmd_text, "استهلاك") || strstr(cmd_text, "باقة") || strstr(cmd_text, "باقي") || strstr(cmd_text, "كوتا") || strstr(cmd_text, "الباقة")) {
                send_admin_quota_report(sender_chat_id, linked_indices, linked_count);
              }
              // 2. Command: /devices, أجهزة, الأجهزة, المتصلين
              else if (strstr(cmd_text, "/devices") || strstr(cmd_text, "الأجهزة") || strstr(cmd_text, "أجهزة") || strstr(cmd_text, "اجهزة") || strstr(cmd_text, "المتصلين") || strstr(cmd_text, "متصل")) {
                send_admin_devices_report(sender_chat_id);
              }
              // 3. Command: /all, الكل
              else if (strstr(cmd_text, "/all") || strstr(cmd_text, "الكل") || strstr(cmd_text, "كافة")) {
                char reply[1200] = "📋 <b>سجل كافة أجهزة الشبكة المسجلة:</b>\n\n";
                portENTER_CRITICAL(&data_mux);
                for (size_t p = 0; p < policy_count; ++p) {
                  char d_name[DEVICE_NAME_BYTES] = "جهاز";
                  int n_idx = meter_find_name_locked(device_policies[p].mac, false);
                  if (n_idx >= 0 && device_names[n_idx].name[0]) {
                    strncpy(d_name, device_names[n_idx].name, sizeof(d_name) - 1);
                  }
                  int u_idx = meter_find_usage_locked(device_policies[p].mac, false);
                  uint64_t dev_used = (u_idx >= 0) ? (usage_records[u_idx].upload_bytes + usage_records[u_idx].download_bytes) : 0;
                  double u_gb = (double)dev_used / 1000000000.0;
                  double q_gb = (double)device_policies[p].quota_bytes / 1000000000.0;

                  char line[150];
                  if (device_policies[p].quota_bytes > 0) {
                    snprintf(line, sizeof(line), "• <b>%s</b>: %.2f / %.2f GB %s%s\n",
                             d_name, u_gb, q_gb,
                             device_policies[p].blocked ? "🔴" : "🟢",
                             device_policies[p].telegram_chat_id[0] ? " [TG]" : "");
                  } else {
                    snprintf(line, sizeof(line), "• <b>%s</b>: %.2f GB %s%s\n",
                             d_name, u_gb,
                             device_policies[p].blocked ? "🔴" : "🟢",
                             device_policies[p].telegram_chat_id[0] ? " [TG]" : "");
                  }
                  if (strlen(reply) + strlen(line) < sizeof(reply) - 30) {
                    strcat(reply, line);
                  }
                }
                portEXIT_CRITICAL(&data_mux);
                telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
              }
              // 4. Command: /block, حظر
              else if ((strstr(cmd_text, "/block") && !strstr(cmd_text, "/unblock")) ||
                       (strstr(cmd_text, "حظر") && !strstr(cmd_text, "فك"))) {
                char *arg = NULL;
                if (strncmp(cmd_text, "/block", 6) == 0) arg = cmd_text + 6;
                else if (strstr(cmd_text, "حظر")) {
                  arg = strstr(cmd_text, "حظر") + strlen("حظر");
                } else {
                  arg = cmd_text;
                }
                while (*arg == ' ') arg++;

                uint8_t target_mac[6] = {0};
                char target_name[DEVICE_NAME_BYTES] = {0};
                uint32_t target_ip = 0;

                if (arg[0] && find_device_by_target(arg, target_mac, target_name, sizeof(target_name), &target_ip)) {
                  meter_set_device_blocked(target_mac, true);
                  char reply[512];
                  ip4_addr_t tip = {.addr = target_ip};
                  snprintf(reply, sizeof(reply),
                           "🚫 <b>تم حظر الجهاز بنجاح!</b>\n\n"
                           "📱 <b>الجهاز:</b> %s\n"
                           "🌐 <b>عنوان IP:</b> %d.%d.%d.%d\n"
                           "⚡ تم قطع وصول الإنترنت فوراً عبر lwIP NAPT hook.",
                           target_name, IP2STR(&tip));
                  telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
                } else {
                  char reply[512];
                  snprintf(reply, sizeof(reply),
                           "⚠️ <b>تعذر العثور على الجهاز المطلوب: '%.32s'</b>\n\n"
                           "يرجى تحديد عنوان IP صحيح للجهاز، مثال:\n"
                           "<code>/block 192.168.4.2</code>\n"
                           "أو كتابة <code>/devices</code> لعرض الأجهزة المتصلة.", arg);
                  telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
                }
              }
              // 5. Command: /unblock, فك_حظر, فك حظر
              else if (strstr(cmd_text, "/unblock") || strstr(cmd_text, "فك_حظر") || strstr(cmd_text, "فك حظر") || strstr(cmd_text, "فك")) {
                char *arg = NULL;
                if (strncmp(cmd_text, "/unblock", 8) == 0) arg = cmd_text + 8;
                else if (strstr(cmd_text, "فك_حظر")) arg = strstr(cmd_text, "فك_حظر") + strlen("فك_حظر");
                else if (strstr(cmd_text, "فك حظر")) arg = strstr(cmd_text, "فك حظر") + strlen("فك حظر");
                else if (strstr(cmd_text, "فك")) arg = strstr(cmd_text, "فك") + strlen("فك");
                else arg = cmd_text;
                while (*arg == ' ') arg++;

                uint8_t target_mac[6] = {0};
                char target_name[DEVICE_NAME_BYTES] = {0};
                uint32_t target_ip = 0;

                if (arg[0] && find_device_by_target(arg, target_mac, target_name, sizeof(target_name), &target_ip)) {
                  meter_set_device_blocked(target_mac, false);
                  char reply[512];
                  ip4_addr_t tip = {.addr = target_ip};
                  snprintf(reply, sizeof(reply),
                           "✅ <b>تم إلغاء حظر الجهاز بنجاح!</b>\n\n"
                           "📱 <b>الجهاز:</b> %s\n"
                           "🌐 <b>عنوان IP:</b> %d.%d.%d.%d\n"
                           "⚡ تمت استعادة اتصال الإنترنت للجهاز بالكامل.",
                           target_name, IP2STR(&tip));
                  telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
                } else {
                  char reply[512];
                  snprintf(reply, sizeof(reply),
                           "⚠️ <b>تعذر العثور على الجهاز المطلوب: '%.32s'</b>\n\n"
                           "يرجى تحديد عنوان IP صحيح للجهاز، مثال:\n"
                           "<code>/unblock 192.168.4.2</code>", arg);
                  telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
                }
              }
              // 6. Command: /speed, سرعة, السرعة
              else if (strstr(cmd_text, "/speed") || strstr(cmd_text, "سرعة") || strstr(cmd_text, "السرعة")) {
                send_admin_speed_report(sender_chat_id, linked_indices, linked_count, &ap_info);
              }
              // 7. Command: /ota, تحديث
              else if (strstr(cmd_text, "/ota") || strstr(cmd_text, "تحديث")) {
                ota_info_t o_inf;
                ota_updater_get_info(&o_inf);
                char reply[650];
                snprintf(reply, sizeof(reply),
                         "🔄 <b>نظام التحديث الهوائي (Dual-Bank OTA):</b>\n\n"
                         "🏷️ <b>الإصدار الحالي:</b> %s\n"
                         "🕒 <b>تاريخ البناء:</b> %s\n"
                         "💾 <b>القسم النشط:</b> %s\n"
                         "🎯 <b>القسم البديل:</b> %s\n"
                         "🛡️ <b>الحماية من الأعطال:</b> Anti-Rollback مفعل\n\n"
                         "💡 <i>يمكنك رفع ملف .bin أو فحص التحديثات السحابية من لوحة تحكم الويب.</i>",
                         o_inf.current_version, o_inf.build_timestamp,
                         o_inf.running_partition, o_inf.target_partition);
                telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
              }
              // 7.5. Command: /add_quota, زيادة باقة
              else if (strncmp(cmd_text, "/add_quota", 10) == 0 || strncmp(cmd_text, "زيادة", strlen("زيادة")) == 0) {
                char *arg = (strncmp(cmd_text, "/add_quota", 10) == 0) ? (cmd_text + 10) : (cmd_text + strlen("زيادة"));
                while (*arg == ' ') arg++;
                char target_str[64] = {0};
                int add_gb = 0;
                if (sscanf(arg, "%63s %d", target_str, &add_gb) == 2 && add_gb >= 0) {
                  uint8_t t_mac[6] = {0};
                  char t_name[DEVICE_NAME_BYTES] = {0};
                  uint32_t t_ip = 0;
                  if (find_device_by_target(target_str, t_mac, t_name, sizeof(t_name), &t_ip)) {
                    double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
                    int64_t add_bytes = (add_gb > 0) ? (int64_t)((double)add_gb * gb_div) : 0;
                    meter_add_device_quota(t_mac, add_bytes);
                    char reply[512];
                    snprintf(reply, sizeof(reply),
                             "✅ <b>تم تحديث باقة الجهاز (%s) بنجاح!</b>\n\n"
                             "📊 <b>الإجراء:</b> %s\n"
                             "🟢 <b>الحالة:</b> تم رفع الحظر تلقائياً واستعادة الإنترنت.",
                             t_name, (add_gb > 0) ? "تمت زيادة الباقة بنجاح" : "تم فتح الباقة بلا حدود ♾️");
                    telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
                  } else {
                    telegram_send_message_with_keyboard(sender_chat_id, "⚠️ لم يتم العثور على الجهاز المطلوب.", TG_ADMIN_KEYBOARD);
                  }
                } else {
                  telegram_send_message_with_keyboard(sender_chat_id,
                      "💡 <b>طريقة الاستخدام:</b>\n<code>/add_quota 192.168.4.2 5</code>\nأو افتح <b>👥 الأجهزة المتصلة</b> واضغط على <b>⚙️ تحكم وباقة وVIP</b>.",
                      TG_ADMIN_KEYBOARD);
                }
              }
              // 7.6. Command: /vip
              else if (strncmp(cmd_text, "/vip", 4) == 0 || strstr(cmd_text, "VIP") || strstr(cmd_text, "vip")) {
                char *arg = cmd_text + 4;
                while (*arg == ' ') arg++;
                uint8_t t_mac[6] = {0};
                char t_name[DEVICE_NAME_BYTES] = {0};
                uint32_t t_ip = 0;
                if (arg[0] && find_device_by_target(arg, t_mac, t_name, sizeof(t_name), &t_ip)) {
                  int p_idx = -1;
                  bool cur_vip = false;
                  portENTER_CRITICAL(&data_mux);
                  p_idx = meter_find_policy_locked(t_mac, true);
                  if (p_idx >= 0) cur_vip = device_policies[p_idx].gaming_mode;
                  portEXIT_CRITICAL(&data_mux);
                  meter_set_device_gaming(t_mac, !cur_vip);
                  char reply[450];
                  snprintf(reply, sizeof(reply),
                           "🎮 <b>وضع الأولوية القصوى (VIP) للجهاز (%s):</b>\n\n"
                           "الحالة الآن: <b>%s</b>",
                           t_name, !cur_vip ? "✅ مفعل (أولوية قصوى وتجاوز حدود السرعة 🚀)" : "⚪ غير مفعل");
                  telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
                } else {
                  send_admin_devices_report(sender_chat_id);
                }
              }
              // 8. Command: /reboot_time HH:MM
              else if (strncmp(cmd_text, "/reboot_time", 12) == 0) {
                const char *arg = cmd_text + 12;
                while (*arg == ' ') arg++;
                int hr = -1, mn = 0;
                if (strncmp(arg, "off", 3) == 0) {
                  config_save_auto_reboot(false, auto_reboot_hour, auto_reboot_min, auto_reboot_tz_offset);
                  meter_reset_reboot_fired_state();
                  send_admin_reboot_menu(sender_chat_id);
                } else if (sscanf(arg, "%d:%d", &hr, &mn) >= 1 && hr >= 0 && hr < 24 && mn >= 0 && mn < 60) {
                  config_save_auto_reboot(true, (uint8_t)hr, (uint8_t)mn, auto_reboot_tz_offset);
                  meter_reset_reboot_fired_state();
                  send_admin_reboot_menu(sender_chat_id);
                } else {
                  telegram_send_message_with_keyboard(sender_chat_id,
                      "⚠️ صيغة الوقت غير صحيحة. أرسل مثلاً:\n<code>/reboot_time 04:30</code> أو <code>/reboot_time 13:00</code>",
                      TG_ADMIN_KEYBOARD);
                }
              }
              // 8.5. Command: /reboot, ريستارت, إعادة التشغيل
              else if (strstr(cmd_text, "/reboot") || strstr(cmd_text, "ريستارت") || strstr(cmd_text, "إعادة التشغيل") || strstr(cmd_text, "اعادة التشغيل") || strstr(cmd_text, "التشغيل")) {
                send_admin_reboot_menu(sender_chat_id);
              }
              // 9. Command: /status, /cpu, حالة, تردد المعالج
              else if (strstr(cmd_text, "/status") || strstr(cmd_text, "/cpu") || strstr(cmd_text, "تردد") || strstr(cmd_text, "المعالج") || strstr(cmd_text, "حالة") || strstr(cmd_text, "الراوتر")) {
                send_admin_status_report(sender_chat_id, &ap_info);
              }
              // 10. Command: /help, /start, مساعدة
              else {
                char reply[1250];
                snprintf(reply, sizeof(reply),
                         "👋 <b>مرحباً بك في لوحة تحكم راوتر ESP32 (المدير)</b> 🌐\n\n"
                         "الأزرار والأوامر الإدارية المتاحة:\n"
                         "📊 <b>استهلاك الباقة</b>: رصيد الباقة وباقات الأجهزة وموعد التجديد\n"
                         "👥 <b>الأجهزة المتصلة</b>: التحكم بالأجهزة، زيادة الباقة (+GB)، تفعيل 🎮 VIP، والحظر\n"
                         "⚡ <b>فحص السرعة</b>: السرعة اللحظية وعرض القناة وقوة الإشارة\n"
                         "⚙️ <b>حالة الراوتر</b> / 🚀 <b>تردد المعالج</b>: الذاكرة وتغيير تردد المعالج (80/160/240 MHz)\n"
                         "🔄 <b>إعادة التشغيل</b>: إعادة التشغيل الفوري أو الجدولة اليومية التلقائية\n"
                         "📋 <b>مساعدة</b>: عرض هذا الدليل الإرشادي\n\n"
                         "💡 <i>أوامر إضافية مباشرة:</i>\n"
                         "• <code>/add_quota [IP] [GB]</code> لزيادة باقة جهاز\n"
                         "• <code>/vip [IP]</code> لتفعيل/إلغاء وضع الـ VIP\n"
                         "• <code>/reboot_time [HH:MM]</code> لضبط وقت ريستارت مخصص");
                telegram_send_message_with_keyboard(sender_chat_id, reply, TG_ADMIN_KEYBOARD);
              }
            }
          }
        }
      }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  free(rx_buffer);
  vTaskDelete(NULL);
}

void telegram_bot_init(void) {
  // Pinned to Core 0 for ESP32-C5 Unicore with 12KB stack to guarantee safe TLS handshake and JSON handling
  xTaskCreatePinnedToCore(telegram_task, "telegram_task", 12288, NULL, 4, NULL, 0);
  ESP_LOGI(TG_TAG, "Telegram bot background task created on Core 0");
}
