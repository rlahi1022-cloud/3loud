#include "client_blacklisthandler.hpp"
#include "protocol.h"
#include "protocol_schema.h"
#include "json_packet.hpp"
#include "packet.h"
#include <iostream>

using json = nlohmann::json;

// --------------------------------------------------
// 블랙리스트 목록 조회
// --------------------------------------------------
void handle_blacklist_list(int sock)
{
    json req = make_request(PKT_BLACKLIST_REQ);
    req["payload"] = {
        {"action", "list"}
    };

    std::string send_str = req.dump();
    packet_send(sock, send_str.c_str(), send_str.size());

    char* buf = nullptr;
    uint32_t len = 0;
    if (packet_recv(sock, &buf, &len) > 0)
    {
        std::string res_str(buf, len);
        std::cout << res_str << std::endl;
        free(buf);
    }
}

// --------------------------------------------------
// 블랙리스트 추가
// --------------------------------------------------
void handle_blacklist_add(int sock)
{
    std::string target;
    std::cout << "차단할 이메일: ";
    std::getline(std::cin, target);

    json req = make_request(PKT_BLACKLIST_REQ);
    req["payload"] = {
        {"action", "add"},
        {"blocked_email", target}
    };

    std::string send_str = req.dump();
    packet_send(sock, send_str.c_str(), send_str.size());

    char* buf = nullptr;
    uint32_t len = 0;
    if (packet_recv(sock, &buf, &len) > 0)
    {
        std::string res_str(buf, len);
        std::cout << res_str << std::endl;
        free(buf);
    }
}

// --------------------------------------------------
// 블랙리스트 삭제
// --------------------------------------------------
void handle_blacklist_remove(int sock)
{
    std::string target;
    std::cout << "해제할 이메일: ";
    std::getline(std::cin, target);

    json req = make_request(PKT_BLACKLIST_REQ);
    req["payload"] = {
        {"action", "remove"},
        {"blocked_email", target}
    };

    std::string send_str = req.dump();
    packet_send(sock, send_str.c_str(), send_str.size());

    char* buf = nullptr;
    uint32_t len = 0;
    if (packet_recv(sock, &buf, &len) > 0)
    {
        std::string res_str(buf, len);
        std::cout << res_str << std::endl;
        free(buf);
    }
}