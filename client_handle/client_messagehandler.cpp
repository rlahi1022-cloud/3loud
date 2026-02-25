#include <iostream>
#include <string>
#include <vector>
#include "protocol.h"
#include "packet.h"
#include "json_packet.hpp"

void clear_stdin_line();

extern std::string g_current_user_email; // 로그인 사용자 전역 변수

// ========================================================
// 메시지 전송
// ========================================================
void handle_message_send_ui(int sock)
{
    std::string receiver, content;

    std::cout << "\n[메시지 전송]\n받는 사람: ";
    std::cin >> receiver;
    clear_stdin_line();

    std::cout << "내용: ";
    std::getline(std::cin, content);

    if (content.length() > 1024)
    {
        std::cout << ">> 오류: 1024바이트 제한 초과\n";
        return;
    }

    json req = make_request(PKT_MSG_SEND_REQ);
    req["payload"] = {
        {"receiver_email", receiver},
        {"content", content},
        {"content_vector", {0.0, 0.0, 0.0}}
    };

    std::string send_str = req.dump();

    if (packet_send(sock, send_str.c_str(),
                    (uint32_t)send_str.size()) < 0)
        return;

    char *recv_buf = nullptr;
    uint32_t recv_len = 0;

    if (packet_recv(sock, &recv_buf, &recv_len) > 0)
    {
        auto res = json::parse(std::string(recv_buf, recv_len));

        if (res.value("code", -1) == VALUE_SUCCESS)
            std::cout << ">> 전송 성공\n";
        else
            std::cout << ">> 전송 실패: "
                      << res.value("msg", "알 수 없는 오류") << "\n";

        free(recv_buf);
    }
}

// ========================================================
// 메시지 목록 조회
// ========================================================
void handle_message_list_ui(int sock)
{
    if (g_current_user_email.empty())
    {
        std::cout << ">> 로그인 정보 없음\n";
        return;
    }

    json req = make_request(PKT_MSG_LIST_REQ);

    req["payload"] = {
        {"email", g_current_user_email} // ★ 핵심 수정
    };

    std::string send_str = req.dump();

    if (packet_send(sock, send_str.c_str(),
                    (uint32_t)send_str.size()) < 0)
        return;

    char *recv_buf = nullptr;
    uint32_t recv_len = 0;

    if (packet_recv(sock, &recv_buf, &recv_len) > 0)
    {
        auto res = json::parse(std::string(recv_buf, recv_len));

        if (res.value("code", -1) == VALUE_SUCCESS)
        {
            auto msgs = res["payload"]["messages"];

            std::cout << "\n--- 메시지 목록 (최근 20개) ---\n";

            for (auto &m : msgs)
            {
                std::cout << "[" << m["sent_at"] << "] "
                          << m["sender"] << ": "
                          << m["content"] << "\n";
            }
        }
        else
        {
            std::cout << ">> 조회 실패: "
                      << res.value("msg", "알 수 없는 오류") << "\n";
        }

        free(recv_buf);
    }
}

// ========================================================
// 메뉴
// ========================================================
void handle_message_menu(int sock)
{
    while (true)
    {
        std::cout << "\n1. 메시지 전송\n";
        std::cout << "2. 메시지 목록 조회\n";
        std::cout << "0. 뒤로가기\n";
        std::cout << "선택: ";

        int sel;
        std::cin >> sel;
        clear_stdin_line();

        if (sel == 1)
            handle_message_send_ui(sock);
        else if (sel == 2)
            handle_message_list_ui(sock);
        else
            break;
    }
}