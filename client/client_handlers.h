#ifndef CLIENT_HANDLERS_H
#define CLIENT_HANDLERS_H

#include <cstdint> // uint32_t를 사용하기 위해 필요
#include <atomic>  // std::atomic을 사용하기 위해 필요
#include <string>  // std::string

extern uint32_t g_user_no;
extern std::string g_current_pw_hash;     // 폴링 소켓 재로그인용
extern std::string g_current_user_email; // 폴링용 이메일
extern std::atomic<bool> g_file_transfer_in_progress;

bool handle_login(int sock);     // 로그인 기능
void handle_signup(int sock);    // 회원가입 기능
void handle_logout(int sock);    // 로그 아웃
void handle_file_list(int sock); //
void handle_file_upload(int sock);
void handle_file_download(int sock);
void handle_message_menu(int sock);
void handle_profile_menu(int sock);

#endif