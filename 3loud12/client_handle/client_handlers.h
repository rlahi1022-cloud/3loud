#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool handle_login(int sock);
void handle_signup(int sock);
void handle_logout(int sock);
void handle_message_menu(int sock);
void handle_profile_menu(int sock);

#ifdef __cplusplus
}
#endif