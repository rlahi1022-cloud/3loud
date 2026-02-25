#ifndef CLIENT_HANDLERS_H
#define CLIENT_HANDLERS_H

bool handle_login(int sock);     // 로그인 기능
void handle_signup(int sock);    // 회원가입 기능
void handle_logout(int sock);    // 로그 아웃
void handle_file_list(int sock); //
void handle_file_upload(int sock);
void handle_file_download(int sock);
void handle_message_menu(int sock);
void handle_profile_menu(int sock);

#endif