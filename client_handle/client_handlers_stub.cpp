// client_handle/client_handlers_stub.cpp
#include "client_handlers.h"
#include <iostream>
#include <cstdint>

// file_client.cpp에 정의된 전역 변수 참조
extern uint32_t g_user_no;

bool handle_login(int)        {
    g_user_no = 1;  // TODO: 실제 로그인 구현 시 서버 응답의 user_no로 교체
    std::cout << "[미구현] 로그인 (임시 user_no=1)\n";
    return true;
}
// void handle_signup(int)       { std::cout << "[미구현] 회원가입\n";  }
// void handle_logout(int)       { g_user_no = 0; std::cout << "[미구현] 로그아웃\n"; }
// void handle_message_menu(int) { std::cout << "[미구현] 메시지\n";    }
void handle_profile_menu(int) { std::cout << "[미구현] 개인설정\n";  }