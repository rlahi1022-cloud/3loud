// client_handle/client_handlers_stub.cpp
#include "client_handlers.h"
#include <iostream>

bool handle_login(int)        { std::cout << "[미구현] 로그인\n";    return true; }
void handle_signup(int)       { std::cout << "[미구현] 회원가입\n";  }
void handle_logout(int)       { std::cout << "[미구현] 로그아웃\n";  }
void handle_message_menu(int) { std::cout << "[미구현] 메시지\n";    }
void handle_profile_menu(int) { std::cout << "[미구현] 개인설정\n";  }