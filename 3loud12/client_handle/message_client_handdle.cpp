#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "protocol.h"      // 
#include "packet.h"        // 송수신 함수용
#include "json_packet.hpp"
#include "message_client_handle.hpp"

using json = nlohmann::json;

// skeleton_client.cpp에 정의된 유틸리티 선언
void clear_stdin_line(); 

// 메시지 송신 UI 핸들러
void handle_message_send_ui(int sock) {
    std::string receiver, content;
    
    std::cout << "\n--- 메시지 보내기 ---\n";
    std::cout << "받는 사람(이메일): "; 
    std::cin >> receiver;
    clear_stdin_line(); [cite: 2]
    
    std::cout << "내용 (최대 1024바이트): ";
    std::getline(std::cin, content);

    // DB 제약 조건(VARCHAR 1024) 체크 
    if (content.length() > 1024) {
        std::cout << ">> [오류] 메시지 크기 초과 (현재: " << content.length() << " bytes)\n";
        return;
    }

    //  프로토콜 규격에 맞춘 패킷 구성
    json req;
    req["type"] = PKT_MSG_SEND_REQ; 
    req["payload"] = {
        {"receiver_email", receiver},
        {"content", content},
        {"content_vector", {0.12, -0.05, 0.88}} // DB의 VECTOR(1536) 대응 
    };

    std::string send_str = req.dump();
    if (send_packet(sock, send_str.c_str(), send_str.size()) < 0) {
        return;
    }

    char* recv_buf = nullptr;
    uint32_t recv_len = 0;
    if (recv_packet(sock, &recv_buf, &recv_len) > 0) {
        try {
            json res = json::parse(std::string(recv_buf, recv_len));
            // protocol.h의 ResultValue 확인 
            int result = res.value("value", res.value("code", -1)); 
            
            if (result == VALUE_SUCCESS) {
                std::cout << ">> [성공] " << res.value("msg", "발송 완료") << "\n";
            } else {
                std::cout << ">> [실패] 코드(" << result << "): " 
                          << res.value("msg", "오류 발생") << "\n";
            }
        } catch (...) {
            std::cerr << ">> [오류] 응답 데이터 해석 실패\n";
        }
        free(recv_buf); // 메모리 해제 권장
    }
}