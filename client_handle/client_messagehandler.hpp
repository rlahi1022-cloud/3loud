#include <iostream>
#include <string>
#include <vector>
#include "protocol.h"
#include "packet.h"
#include "json_packet.hpp"

void clear_stdin_line();

// 송신 UI 핸들러
void handle_message_send_ui(int sock) {
    std::string receiver, content;
    
    std::cout << "\n[메시지 전송]\n받는 사람: "; 
    std::cin >> receiver;
    clear_stdin_line();
    
    std::cout << "내용: ";
    std::getline(std::cin, content);

    if (content.length() > 1024) {
        std::cout << ">> 오류: 1024바이트 제한 초과\n";
        return;
    }

    json req = make_request(PKT_MSG_SEND_REQ);
    req["payload"] = {
        {"receiver_email", receiver},
        {"content", content},
        {"content_vector", {0.0, 0.0, 0.0}} // JSON 컬럼 데이터 예시
    };

    std::string send_str = req.dump();
    // packet.h의 규격인 packet_send 사용
    if (packet_send(sock, send_str.c_str(), (uint32_t)send_str.size()) < 0) {
        return;
    }

    char* recv_buf = nullptr;
    uint32_t recv_len = 0;
    if (packet_recv(sock, &recv_buf, &recv_len) > 0) {
        auto res = json::parse(std::string(recv_buf, recv_len));
        if (res.value("code", -1) == VALUE_SUCCESS) {
            std::cout << ">> 전송 성공\n";
        }
        free(recv_buf);
    }
}

// 목록 조회 UI 핸들러
void handle_message_list_ui(int sock) {
    json req = make_request(PKT_MSG_LIST_REQ);
    std::string send_str = req.dump();
    
    if (packet_send(sock, send_str.c_str(), (uint32_t)send_str.size()) < 0) return;

    char* recv_buf = nullptr;
    uint32_t recv_len = 0;
    if (packet_recv(sock, &recv_buf, &recv_len) > 0) {
        auto res = json::parse(std::string(recv_buf, recv_len));
        if (res.value("code", -1) == VALUE_SUCCESS) {
            auto msgs = res["payload"]["messages"];
            std::cout << "\n--- 메시지 목록 (최근 20개) ---\n";
            for (auto& m : msgs) {
                std::cout << "[" << m["sent_at"] << "] " << m["sender"] << ": " << m["content"] << "\n";
            }
        }
        free(recv_buf);
    }
}