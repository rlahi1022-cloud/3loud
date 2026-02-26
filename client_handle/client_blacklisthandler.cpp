#include "client_blacklisthandler.hpp"
#include "protocol.h"
#include "protocol_schema.h"
#include "json_packet.hpp"
#include "packet.h"
#include "tui.hpp"
#include <iostream>

using json = nlohmann::json;

// --------------------------------------------------
// 블랙리스트 목록 조회
// --------------------------------------------------
void handle_blacklist_list(int sock)
{
    json req = make_request(PKT_BLACKLIST_REQ);
    req["payload"] = { {"action", "list"} };

    std::string send_str = req.dump();
    packet_send(sock, send_str.c_str(), send_str.size());
    printf("### handle_blacklist_list 진입 ###\n");
    char* buf = nullptr;
    uint32_t len = 0;

    if (packet_recv(sock, &buf, &len) <= 0)
        return;

    std::string res_str(buf, len);
    free(buf);

    json res;
    try {
        res = json::parse(res_str);
    } catch (...) {
        std::vector<std::string> err = {
            "JSON 파싱 실패",
            "뒤로가기"
        };
        (void)tui_menu("오류", err);
        return;
    }

    if (!res.contains("code") || res["code"] != VALUE_SUCCESS)
    {
        std::vector<std::string> err = {
            res.value("msg", "알 수 없는 오류"),
            "뒤로가기"
        };
        (void)tui_menu("오류", err);
        return;
    }

    std::vector<std::string> items;

    if (!res.contains("payload") ||
        !res["payload"].contains("list") ||
        !res["payload"]["list"].is_array() ||
        res["payload"]["list"].empty())
    {
        items.push_back("차단된 사용자가 없습니다.");
    }
    else
    {
        for (const auto& item : res["payload"]["list"])
        {
            std::string line =
                item.value("blocked_email", "") +
                " | " +
                item.value("created_at", "");

            items.push_back(line);
        }
    }

    items.push_back("뒤로가기");

    // 반환값을 명시적으로 무시
    (void)tui_menu("블랙리스트 목록", items);
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