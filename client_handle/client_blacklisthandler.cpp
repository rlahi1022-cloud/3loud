#include "client_blacklisthandler.hpp"
#include "protocol.h"
#include "protocol_schema.h"
#include "json_packet.hpp"
#include "packet.h"
#include "tui.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <limits>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 내부 헬퍼: 블랙리스트 항목 구조체
// ──────────────────────────────────────────────────────
struct BlacklistEntry {
    std::string blocked_email;
    std::string created_at;
};

// 서버에서 블랙리스트 목록 가져오기
static std::vector<BlacklistEntry> fetch_blacklist(int sock)
{
    std::vector<BlacklistEntry> result;

    json req = make_request(PKT_BLACKLIST_REQ);
    req["payload"] = { {"action", "list"} };
    std::string send_str = req.dump();
    if (packet_send(sock, send_str.c_str(), send_str.size()) < 0)
        return result;

    char* buf = nullptr;
    uint32_t len = 0;
    if (packet_recv(sock, &buf, &len) < 0)
        return result;

    std::string res_str(buf, len);
    free(buf);

    try {
        json res = json::parse(res_str);
        if (!res.contains("code") || res["code"] != VALUE_SUCCESS)
            return result;
        if (!res.contains("payload") || !res["payload"].contains("list"))
            return result;
        for (const auto& item : res["payload"]["list"]) {
            BlacklistEntry e;
            e.blocked_email = item.value("blocked_email", "");
            e.created_at    = item.value("created_at", "");
            if (!e.blocked_email.empty())
                result.push_back(e);
        }
    } catch (...) {}

    return result;
}

// ──────────────────────────────────────────────────────
// 1. 블랙리스트 확인하기
//    목록을 tui_menu로 표시, 항목 선택 시 차단 날짜 팝업
// ──────────────────────────────────────────────────────
void handle_blacklist_list(int sock)
{
    auto entries = fetch_blacklist(sock);

    if (entries.empty()) {
        tui_menu("블랙리스트 목록", {"차단된 사용자가 없습니다.", "뒤로가기"});
        return;
    }

    std::vector<std::string> items;
    for (const auto& e : entries)
        items.push_back(e.blocked_email);
    items.push_back("뒤로가기");

    while (true) {
        int sel = tui_menu(
            "블랙리스트 목록  (" + std::to_string(entries.size()) + "명 차단 중)",
            items
        );

        if (sel == -1 || sel == (int)items.size() - 1)
            break;

        // 선택한 항목의 차단 날짜 팝업
        std::string detail = entries[sel].blocked_email
                           + "\n  차단 일시: "
                           + entries[sel].created_at;
        tui_menu(detail, {"확인"});
    }
}

// ──────────────────────────────────────────────────────
// 2. 블랙리스트 추가하기
// ──────────────────────────────────────────────────────
void handle_blacklist_add(int sock)
{
    system("clear");
    printf("============================================================\n");
    printf("  블랙리스트 추가\n");
    printf("============================================================\n");
    printf("  차단할 이메일을 입력하세요 (취소: /c)\n");
    printf("------------------------------------------------------------\n");

    // tui_menu 이후 남은 stdin 버퍼 비우기
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    std::string target;
    while (true) {
        printf("  이메일 > ");
        fflush(stdout);
        std::getline(std::cin, target);

        if (target == "/c")
            return;
        if (!target.empty())
            break;
        printf("  >> 이메일을 입력해주세요.\n");
    }

    json req = make_request(PKT_BLACKLIST_REQ);
    req["payload"] = { {"action", "add"}, {"blocked_email", target} };
    std::string send_str = req.dump();

    if (packet_send(sock, send_str.c_str(), send_str.size()) < 0) {
        tui_menu("서버 전송 실패", {"확인"});
        return;
    }

    char* buf = nullptr;
    uint32_t len = 0;
    if (packet_recv(sock, &buf, &len) < 0) {
        tui_menu("서버 응답 없음", {"확인"});
        return;
    }
    std::string res_str(buf, len);
    free(buf);

    try {
        json res = json::parse(res_str);
        int code = res.value("code", -1);
        if (code == VALUE_SUCCESS)
            tui_menu(target + " 차단 완료", {"확인"});
        else if (code == VALUE_ERR_ID_DUPLICATE)
            tui_menu("이미 차단된 사용자입니다.", {"확인"});
        else
            tui_menu(res.value("msg", "추가 실패"), {"확인"});
    } catch (...) {
        tui_menu("응답 파싱 오류", {"확인"});
    }
}

// ──────────────────────────────────────────────────────
// 3. 블랙리스트 삭제하기
//    현재 목록을 방향키 메뉴로 표시 → 선택 → 삭제 확인 → 서버 요청
// ──────────────────────────────────────────────────────
void handle_blacklist_remove(int sock)
{
    while (true) {
        auto entries = fetch_blacklist(sock);

        if (entries.empty()) {
            tui_menu("차단된 사용자가 없습니다.", {"확인"});
            return;
        }

        std::vector<std::string> items;
        for (const auto& e : entries)
            items.push_back(e.blocked_email + "  (" + e.created_at + ")");
        items.push_back("뒤로가기");

        int sel = tui_menu("삭제할 사용자를 선택하세요", items);

        if (sel == -1 || sel == (int)items.size() - 1)
            return;

        const std::string target_email = entries[sel].blocked_email;

        // 삭제 확인 팝업
        int confirm = tui_menu(
            target_email + "\n  차단을 해제하시겠습니까?",
            {"취소", "해제"}
        );
        if (confirm != 1)
            continue;

        // 서버에 remove 요청
        json req = make_request(PKT_BLACKLIST_REQ);
        req["payload"] = { {"action", "remove"}, {"blocked_email", target_email} };
        std::string send_str = req.dump();

        if (packet_send(sock, send_str.c_str(), send_str.size()) < 0) {
            tui_menu("서버 전송 실패", {"확인"});
            return;
        }

        char* buf = nullptr;
        uint32_t len = 0;
        if (packet_recv(sock, &buf, &len) < 0) {
            tui_menu("서버 응답 없음", {"확인"});
            return;
        }
        std::string res_str(buf, len);
        free(buf);

        try {
            json res = json::parse(res_str);
            if (res.value("code", -1) == VALUE_SUCCESS)
                tui_menu(target_email + " 차단 해제 완료", {"확인"});
            else
                tui_menu(res.value("msg", "해제 실패"), {"확인"});
        } catch (...) {
            tui_menu("응답 파싱 오류", {"확인"});
        }
        // 삭제 후 목록 새로고침
    }
}
