#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
  char chat_id[32];
  char first_name[48];
  char username[32];
  uint32_t last_seen_s;
} tg_discovered_user_t;

#define MAX_DISCOVERED_TG_USERS 16

void telegram_bot_init(void);
void telegram_check_quota_alerts(void);
void telegram_reset_alert_flags(void);
bool telegram_send_test_alert(void);
int telegram_send_test_alert_diag(char *out_err, size_t max_err);
bool telegram_send_message(const char *chat_id, const char *text);
bool telegram_send_message_with_keyboard(const char *chat_id, const char *text, const char *keyboard_json);
bool telegram_answer_callback_query(const char *callback_query_id, const char *text);

bool telegram_is_admin(const char *chat_id);
void telegram_broadcast_admins(const char *text);
void telegram_register_discovered_user(const char *chat_id, const char *first_name, const char *username);
size_t telegram_get_discovered_users(tg_discovered_user_t *out_users, size_t max_count);
