// ============================================================================
// 파일명: client_net.cpp
// 설명: length-prefix 기반 JSON 송수신 구현
// ============================================================================

#include "client_net.hpp"
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

int connect_server(const std::string &ip, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0); // TCP 소켓 생성
    if (sock < 0)
        return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;                      // IPv4
    addr.sin_port = htons(port);                    // 포트 설정
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr); // IP 변환

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}

bool send_json(int sock, const json &j)
{
    // 설명: "인코딩 에러가 나면 멈추지 말고, 깨진 글자를  같은 걸로 바꿔서라도 계속 진행해라"
    std::string payload = j.dump(-1, ' ', false, json::error_handler_t::replace);

    uint32_t len = payload.size(); // 길이 계산
    uint32_t net_len = htonl(len); // 네트워크 바이트 순서 변환

    // 길이 먼저 전송
    if (send(sock, &net_len, sizeof(net_len), 0) != sizeof(net_len))
        return false;

    // JSON 본문 전송
    if (send(sock, payload.c_str(), payload.size(), 0) != (ssize_t)payload.size())
        return false;

    return true;
}

bool recv_json(int sock, json &j)
{
    uint32_t net_len;

    // 길이 4바이트 수신
    if (recv(sock, &net_len, sizeof(net_len), MSG_WAITALL) != sizeof(net_len))
        return false;

    uint32_t len = ntohl(net_len); // 길이 복원

    std::string buffer(len, '\0');

    // payload 수신
    if (recv(sock, buffer.data(), len, MSG_WAITALL) != (ssize_t)len)
        return false;

    try
    {
        j = json::parse(buffer); // JSON 파싱
    }
    catch (...)
    {
        return false;
    }

    return true;
}