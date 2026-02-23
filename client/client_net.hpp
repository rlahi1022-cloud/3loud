// ============================================================================
// 파일명: client_net.hpp
// 설명: 서버와 length-prefix 기반 통신을 담당하는 공통 모듈
// ============================================================================

#pragma once
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// 서버 연결 함수
int connect_server(const std::string& ip, int port);

// JSON 전송 함수 (length-prefix 기반)
bool send_json(int sock, const json& j);

// JSON 수신 함수 (length-prefix 기반)
bool recv_json(int sock, json& j);