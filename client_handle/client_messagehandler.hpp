#pragma once

void load_receiver_history();   // 로그인 후 호출 (이력 파일 로드)
void handle_message_send_ui(int sock);
void handle_message_list_ui(int sock);
void handle_message_menu(int sock);
