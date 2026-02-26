// ============================================================
// client_messagehandler.cpp
//
// 요구사항 구현 목록:
//   11-1.   메시지 보내기
//   11-1-1. 전송 전 미리보기 확인창
//   11-1-2. 수신자 이력 (최대 10개, 클라이언트 로컬 저장)
//   11-1-3. 최대 1024 bytes (기본/마무리 메시지 포함)
//   11-2.   메시지 확인하기 (읽음/안읽음 구분, 20개씩, 최신순)
//   11-3.   메시지 삭제하기 (목록에서 선택)
//   10-1.   안읽은 메시지 있으면 메뉴에서 표시
// ============================================================

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <termios.h>
#include <unistd.h>
#include "tui.hpp"
#include "protocol.h"
#include "packet.h"
#include "json_packet.hpp"
#include "protocol_schema.h"

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 전역 상태
// ──────────────────────────────────────────────

// 로그인한 사용자 이메일
extern std::string g_current_user_email;

// 메시지 설정 (13-2-1, 13-2-2): 설정 메뉴에서 변경 가능
std::string g_msg_prefix;   // 기본 메시지
std::string g_msg_suffix;   // 마무리 메시지

// 수신자 이력 (11-1-2): 클라이언트 로컬, 최대 10개
static std::vector<std::string> g_receiver_history;

// ──────────────────────────────────────────────
// 유틸: stdin 한 줄 비우기
// ──────────────────────────────────────────────
extern void clear_stdin_line();

// ──────────────────────────────────────────────
// 유틸: 수신자 이력에 추가 (중복 제거, 최신 우선, 최대 10개)
// ──────────────────────────────────────────────
static void push_receiver_history(const std::string& email)
{
    // 기존에 있으면 제거
    g_receiver_history.erase(
        std::remove(g_receiver_history.begin(),
                    g_receiver_history.end(), email),
        g_receiver_history.end()
    );
    // 앞에 삽입
    g_receiver_history.insert(g_receiver_history.begin(), email);
    // 최대 10개 유지
    if (g_receiver_history.size() > 10)
        g_receiver_history.resize(10);
}

// ──────────────────────────────────────────────
// 유틸: 패킷 전송 + 응답 수신 (공통)
// ──────────────────────────────────────────────
static json send_recv(int sock, const json& req)
{
    std::string send_str = req.dump();
    if (packet_send(sock, send_str.c_str(), (uint32_t)send_str.size()) < 0)
        return json{{"code", VALUE_ERR_UNKNOWN}, {"msg", "전송 실패"}};

    char*    recv_buf = nullptr;
    uint32_t recv_len = 0;

    if (packet_recv(sock, &recv_buf, &recv_len) < 0)
        return json{{"code", VALUE_ERR_UNKNOWN}, {"msg", "수신 실패"}};

    json res = json::parse(std::string(recv_buf, recv_len));
    free(recv_buf);
    return res;
}

// ============================================================
// 메시지 보내기 (11-1 ~ 11-1-3)
// ============================================================
static void handle_message_send_ui(int sock)
{
    std::cout << "\n[메시지 전송]\n";

    // ── 수신자 입력 (11-1-2: ↑ 키 이력 안내) ──
    std::string receiver;
    std::cout << "받는 사람 이메일 (이전 이력 보기: h 입력): ";
    std::cin >> receiver;
    clear_stdin_line();

    if (receiver == "h" || receiver == "H")
    {
        if (g_receiver_history.empty())
        {
            tui_menu("이전 수신자 이력 없음", {"확인"});
            return;
        }

        // 이력 목록을 tui_menu로 표시
        std::vector<std::string> hist_items(
            g_receiver_history.begin(), g_receiver_history.end());
        hist_items.push_back("취소");

        int hist_sel = tui_menu("수신자 이력 선택", hist_items);
        int cancel_idx = (int)g_receiver_history.size();

        if (hist_sel == -1 || hist_sel == cancel_idx) return;
        receiver = g_receiver_history[hist_sel];
    }

    if (receiver.empty())
    {
        tui_menu("수신자를 입력하세요", {"확인"});
        return;
    }

    // ── 메시지 입력 ──
    std::cout << "내용: ";
    std::string content;
    std::getline(std::cin, content);

    // ── 기본/마무리 메시지 조합 (13-2-1, 13-2-2) ──
    std::string full_content;
    if (!g_msg_prefix.empty()) full_content += g_msg_prefix;
    full_content += content;
    if (!g_msg_suffix.empty()) full_content += g_msg_suffix;

    // ── 길이 검증 (11-1-3) ──
    if (full_content.size() > 1024)
    {
        std::cout << ">> 오류: 기본/마무리 메시지 포함 1024 bytes 초과 ("
                  << full_content.size() << " bytes)\n";
        return;
    }

    // ── 미리보기 확인창 (11-1-1) ──
    std::string preview =
        "수신자: " + receiver + "\n\n" + full_content;

    int confirm = tui_menu(preview, {"취소", "전송"});
    if (confirm != 1) return;   // 취소 또는 ESC

    // ── 수신자 이력에 추가 ──
    push_receiver_history(receiver);

    // ── 패킷 전송 ──
    json req = MessageSchema::make_send_req(PKT_MSG_SEND_REQ,
                                            receiver,
                                            full_content);
    json res = send_recv(sock, req);

    if (res.value("code", -1) == VALUE_SUCCESS)
        tui_menu("전송 완료!", {"확인"});
    else
        tui_menu("전송 실패: " + res.value("msg", std::string("알 수 없는 오류")), {"확인"});
}

// ============================================================
// 메시지 확인하기 (11-2 ~ 11-2-2)
// ============================================================
static bool handle_message_list_ui(int sock)
{
    int  page        = 0;
    bool last_unread = false;

    while (true)
    {
        // ── 서버에서 메시지 목록 조회 ──
        json req = MessageSchema::make_list_req(PKT_MSG_LIST_REQ);
        req["payload"]["page"] = page;
        json res = send_recv(sock, req);

        if (res.value("code", -1) != VALUE_SUCCESS)
        {
            tui_menu("오류: " + res.value("msg", "조회 실패"), {"확인"});
            return false;
        }

        auto& payload    = res["payload"];
        auto  msgs       = payload["messages"];   // copy (참조면 루프 중 수정 위험)
        bool  has_unread = payload.value("has_unread", false);
        last_unread = has_unread;

        if (msgs.empty())
        {
            if (page > 0) { page--; continue; }
            tui_menu("메시지 없음", {"확인"});
            return last_unread;
        }

        // ── tui_menu용 항목 생성 ──
        std::vector<std::string> items;
        for (auto& m : msgs)
        {
            bool        is_read = m.value("is_read", false);
            std::string mark    = is_read ? "    " : "[NEW]";
            std::string date    = m.value("sent_at", "");
            if (date.size() > 16) date = date.substr(0, 16);  // "YYYY-MM-DD HH:MM"
            std::string from    = m.value("from_email", "");
            std::string body    = m.value("content", "");
            if (body.size() > 30) body = body.substr(0, 30) + "...";

            items.push_back(mark + " [" + date + "] " + from + "  " + body);
        }

        // 페이지 네비게이션 항목 추가
        if (page > 0)        items.push_back("◀ 이전 페이지");
        items.push_back("▶ 다음 페이지");
        items.push_back("뒤로가기");

        std::string title = "메시지 목록 (페이지 " + std::to_string(page + 1) + ")";
        if (has_unread) title += "  \033[33m[!] 읽지 않은 메시지 있음\033[0m";

        int sel = tui_menu(title, items);

        // ── 선택 처리 ──
        int msg_count  = (int)msgs.size();
        int prev_idx   = (page > 0) ? msg_count       : -1;
        int next_idx   = (page > 0) ? msg_count + 1   : msg_count;
        int back_idx   = (page > 0) ? msg_count + 2   : msg_count + 1;

        if (sel == -1 || sel == back_idx) break;          // ESC / 뒤로가기
        if (sel == next_idx) { page++; continue; }        // 다음 페이지
        if (page > 0 && sel == prev_idx) { page--; continue; }  // 이전 페이지

        if (sel >= 0 && sel < msg_count)
        {
            // 메시지 상세 보기 + 읽음 처리
            auto& m = msgs[sel];
            int   msg_id  = m.value("msg_id", 0);
            bool  is_read = m.value("is_read", false);

            std::string detail =
                "From: " + m.value("from_email", "") + "\n" +
                "시간: " + m.value("sent_at", "")    + "\n\n" +
                m.value("content", "");

            tui_menu(detail, {"확인"});

            // 읽지 않은 메시지면 읽음 처리
            if (!is_read)
            {
                json read_req = MessageSchema::make_read_req(PKT_MSG_READ_REQ, msg_id);
                send_recv(sock, read_req);
                last_unread = false;  // 낙관적 업데이트
            }
        }
    }
    return last_unread;
}

static void handle_message_delete_ui(int sock)
{
    // ── 목록 조회 ──
    json list_req = MessageSchema::make_list_req(PKT_MSG_LIST_REQ);
    list_req["payload"]["page"] = 0;
    json list_res = send_recv(sock, list_req);

    if (list_res.value("code", -1) != VALUE_SUCCESS)
    {
        tui_menu("오류: " + list_res.value("msg", "조회 실패"), {"확인"});
        return;
    }

    auto msgs = list_res["payload"]["messages"];

    if (msgs.empty())
    {
        tui_menu("삭제할 메시지 없음", {"확인"});
        return;
    }

    // ── tui_menu용 항목 생성 ──
    std::vector<std::string> items;
    for (auto& m : msgs)
    {
        std::string date = m.value("sent_at", "");
        if (date.size() > 16) date = date.substr(0, 16);
        std::string from = m.value("from_email", "");
        std::string body = m.value("content", "");
        if (body.size() > 30) body = body.substr(0, 30) + "...";
        items.push_back("[" + date + "] " + from + "  " + body);
    }
    items.push_back("취소");

    int sel = tui_menu("삭제할 메시지 선택", items);

    int cancel_idx = (int)msgs.size();
    if (sel == -1 || sel == cancel_idx) return;
    if (sel < 0 || sel >= (int)msgs.size()) return;

    int         msg_id = msgs[sel].value("msg_id", 0);
    std::string from   = msgs[sel].value("from_email", "");
    std::string body   = msgs[sel].value("content", "");
    if (body.size() > 40) body = body.substr(0, 40) + "...";

    // ── 삭제 확인 ──
    int confirm = tui_menu(
        "삭제하시겠습니까?\n  " + from + "  " + body,
        {"취소", "삭제"}
    );
    if (confirm != 1) return;

    // ── 삭제 요청 ──
    json del_req;
    del_req["type"]    = PKT_MSG_DELETE_REQ;
    del_req["payload"] = {{"msg_ids", json::array({msg_id})}};

    json del_res = send_recv(sock, del_req);

    if (del_res.value("code", -1) == VALUE_SUCCESS)
        tui_menu("삭제 완료", {"확인"});
    else
        tui_menu("삭제 실패: " + del_res.value("msg", "오류"), {"확인"});
}


// ============================================================
// 메시지 메뉴 (10-1: 안읽은 메시지 있으면 표시)
// ============================================================
void handle_message_menu(int sock)
{
    while (true)
    {
        // ── 메뉴 진입마다 안읽은 메시지 여부 서버 폴링 ──
        bool has_unread = false;
        {
            json poll_req = make_request(PKT_MSG_LIST_REQ);
            poll_req["payload"]["page"] = 0;
            std::string s = poll_req.dump();

            if (packet_send(sock, s.c_str(), (uint32_t)s.size()) == 0)
            {
                char*    rbuf = nullptr;
                uint32_t rlen = 0;
                if (packet_recv(sock, &rbuf, &rlen) == 0)
                {
                    auto r = json::parse(std::string(rbuf, rlen));
                    has_unread = r["payload"].value("has_unread", false);
                    free(rbuf);
                }
            }
        }

        // 안읽은 메시지 여부를 항목에 반영
        std::string read_label = has_unread
            ? "메시지 확인하기  \033[33m[!]\033[0m"
            : "메시지 확인하기";

        int sel = tui_menu("메시지 메뉴", {
            "메시지 보내기",
            read_label,
            "메시지 삭제하기",
            "뒤로가기"
        });

        if (sel == -1 || sel == 3) return;   // ESC 또는 뒤로가기

        if (sel == 0) { handle_message_send_ui(sock);   }
        if (sel == 1) { handle_message_list_ui(sock);   }
        if (sel == 2) { handle_message_delete_ui(sock); }
    }
}

// ============================================================
// 메시지 설정 메뉴 (13-2)
// ============================================================
void handle_message_settings(int sock)
{
    while (true)
    {
        int sel = tui_menu("메시지 설정", {
            "기본 메시지 설정",
            "마무리 메시지 설정",
            "블랙리스트 관리",
            "뒤로가기"
        });

        if (sel == -1 || sel == 3) return;

        // ─────────────────────────────
        // 1. 기본 메시지 설정 (prefix)
        // ─────────────────────────────
        if (sel == 0)
        {
            std::cout << "앞에 자동으로 붙을 메시지 입력: ";
            std::string input;
            std::getline(std::cin, input);

            g_msg_prefix = input;

            tui_menu("기본 메시지 설정 완료", {"확인"});
        }

        // ─────────────────────────────
        // 2. 마무리 메시지 설정 (suffix)
        // ─────────────────────────────
        if (sel == 1)
        {
            std::cout << "뒤에 자동으로 붙을 메시지 입력: ";
            std::string input;
            std::getline(std::cin, input);

            g_msg_suffix = input;

            tui_menu("마무리 메시지 설정 완료", {"확인"});
        }

        // ─────────────────────────────
        // 3. 블랙리스트 관리
        // ─────────────────────────────
        if (sel == 2)
        {
            tui_menu("블랙리스트 기능은 서버 연동 필요", {"확인"});
        }
    }
}