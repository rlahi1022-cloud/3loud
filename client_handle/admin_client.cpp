
#include "admin_client.hpp"
#include "client_net.hpp"  // send_json, recv_json
#include "protocol.h"      // PKT_ADMIN_*, VALUE_SUCCESS 등 패킷 매크로
#include "json_packet.hpp" // make_request, json 라이브러리
#include "tui.hpp"         // tui_menu, tui_detail 유틸

#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;
extern uint32_t g_user_no;

void admin_show_user_list(int sock, bool only_inactive)
{
    while (true)
    {
        // 1. 목록 요청
        json req = make_request(PKT_ADMIN_USER_LIST_REQ);
        req["payload"]["only_inactive"] = only_inactive;
        if (!send_json(sock, req))
            return;

        json res;
        if (!recv_json(sock, res) || res["code"] != VALUE_SUCCESS)
            return;

        json users = res["payload"]["users"];
        if (users.empty())
        {
            tui_detail::clear();
            std::cout << "==========================================\n";
            std::cout << "  " << (only_inactive ? "비활성화된 유저 목록" : "전체 유저 목록") << "\n";
            std::cout << "------------------------------------------\n";
            std::cout << "  해당 조건에 맞는 유저가 없습니다.\n";
            std::cout << "==========================================\n";
            std::cout << "계속하려면 Enter...";
            std::cin.get();
            return;
        }

        // 2. TUI 목록 구성 (접속 여부 색상 적용)
        std::vector<std::string> items;
        for (auto &u : users)
        {
            std::string line = u["email"].get<std::string>() + " (" + u["nickname"].get<std::string>() + ")";
            if (u["is_online"] == true)
                line += " \033[32m[온라인]\033[0m"; // 초록색
            else
                line += " \033[90m[오프라인]\033[0m"; // 회색
            items.push_back(line);
        }
        items.push_back("뒤로 가기");

        int choice = tui_menu(only_inactive ? "비활성화된 유저 목록" : "전체 접속 유저 목록", items);
        if (choice == -1 || choice == (int)items.size() - 1)
            break;

        // 3. 상세 정보 요청 및 표시
        int target_no = users[choice]["no"];
        json info_req = make_request(PKT_ADMIN_USER_INFO_REQ);
        info_req["payload"]["target_no"] = target_no;
        send_json(sock, info_req);
        json info_res;
        recv_json(sock, info_res);

        // 서버 응답이 에러일 경우 튕기지 않고 메시지 출력
        if (info_res.value("code", -1) != VALUE_SUCCESS)
        {
            std::cout << "\n[서버 오류] " << info_res.value("msg", "상세 정보 조회 실패") << "\n";
            std::cout << "계속하려면 Enter...";
            std::cin.get();
            continue; // 프로그램 종료 없이 목록으로 돌아감
        }

        auto info = info_res["payload"];
        tui_detail::clear();
        std::cout << "==========================================\n";
        std::cout << "  유저 상세 정보\n";
        std::cout << "------------------------------------------\n";
        std::cout << "  ID (Email)  : " << info["email"].get<std::string>() << "\n";
        std::cout << "  User No     : " << info["no"] << "\n";
        std::cout << "  닉네임      : " << info["nickname"].get<std::string>() << "\n";
        std::cout << "  가입일시    : " << info["created_at"].get<std::string>() << "\n";
        std::cout << "  등급        : " << info["grade"] << "\n";
        std::cout << "  사용 용량   : " << tui_detail::human_size(info["storage_used"]) << "\n";
        std::cout << "  상태        : " << (info["is_active"] == 1 ? "활성" : "\033[31m비활성\033[0m") << "\n";
        std::cout << "==========================================\n";
        std::cout << "  [ESC] 뒤로가기";

        if (info["is_active"] == 1)
            std::cout << "   ['b'] 계정 비활성화\n";
        else
            std::cout << "   ['U'] 계정 활성화\n";

        // 4. 단축키 입력 처리
        termios old_t;
        tui_detail::set_raw(old_t);
        tui_detail::hide_cursor();
        while (true)
        {
            int k = tui_detail::read_key();
            if (k == 27 || k == 'q')
                break; // ESC
            if ((k == 'b' || k == 'B') && info["is_active"] == 1)
            { // 비활성화
                json state_req = make_request(PKT_ADMIN_STATE_CHANGE_REQ);
                state_req["payload"] = {{"target_no", target_no}, {"is_active", 0}};
                send_json(sock, state_req);
                recv_json(sock, state_req);

                // [추가] 성공 메시지 출력 후 대기
                std::cout << "\n\n  >> \033[32m[성공]\033[0m 계정을 비활성화했습니다.\n  아무 키나 누르세요...";
                fflush(stdout);
                tui_detail::read_key(); // 사용자가 키를 누를 때까지 대기
                break;
            }
            if ((k == 'u' || k == 'U') && info["is_active"] == 0)
            { // 활성화
                json state_req = make_request(PKT_ADMIN_STATE_CHANGE_REQ);
                state_req["payload"] = {{"target_no", target_no}, {"is_active", 1}};
                send_json(sock, state_req);
                recv_json(sock, state_req);

                // [추가] 성공 메시지 출력 후 대기
                std::cout << "\n\n  >> \033[32m[성공]\033[0m 계정을 활성화했습니다.\n  아무 키나 누르세요...";
                fflush(stdout);
                tui_detail::read_key(); // 사용자가 키를 누를 때까지 대기
                break;
            }
        } // <-- [내부 루프] 단축키 입력 while 문 끝

        // ★ 터미널 복구 코드가 반드시 바깥 루프가 끝나기 전에 있어야 합니다!
        tui_detail::show_cursor();
        tui_detail::restore_raw(old_t);

    } // <-- [바깥 루프] 전체 유저 목록 while 문 끝
} // <-- admin_show_user_list 함수 끝

void admin_broadcast_message(int sock)
{
    // 1. 전체 유저 로드
    json req = make_request(PKT_ADMIN_USER_LIST_REQ);
    req["payload"]["only_inactive"] = false;
    send_json(sock, req);
    json res;
    recv_json(sock, res);
    json users = res["payload"]["users"];

    std::vector<bool> selected(users.size(), false);
    int cur = 0, n = users.size(), offset = 0;

    // 2. 다중 선택 커스텀 TUI
    termios old_t;
    tui_detail::set_raw(old_t);
    tui_detail::hide_cursor();
    while (true)
    {
        tui_detail::clear();
        std::cout << "========== 모든 유저에게 메시지 보내기 ==========\n";
        int vsz = tui_detail::viewport_size(4, 3);
        tui_detail::adjust_offset(cur, n, vsz, offset);

        for (int i = offset; i < std::min(offset + vsz, n); i++)
        {
            std::string prefix = selected[i] ? "\033[32m[X]\033[0m " : "[ ] ";
            tui_detail::print_item(prefix + users[i]["email"].get<std::string>(), i == cur);
        }
        std::cout << "-------------------------------------------------\n";
        std::cout << " [↑↓] 이동  [Enter] 개별 선택  ['a'] 전체 선택\n";
        std::cout << " ['m'] 선택 완료 및 메시지 작성  [ESC] 취소\n";

        int k = tui_detail::read_key();
        if (k == 27 || k == 'q')
        {
            tui_detail::show_cursor();
            tui_detail::restore_raw(old_t);
            return;
        }
        else if (k == 1000)
            cur = (cur - 1 + n) % n;
        else if (k == 1001)
            cur = (cur + 1) % n;
        else if (k == '\n' || k == '\r')
            selected[cur] = !selected[cur];
        else if (k == 'a' || k == 'A')
            std::fill(selected.begin(), selected.end(), true);
        else if (k == 'm' || k == 'M')
            break;
    }
    tui_detail::show_cursor();
    tui_detail::restore_raw(old_t);

    // 3. 메시지 작성 및 전송 (기존 PKT_MSG_SEND_REQ 재사용)
    tui_detail::clear();
    std::cout << "\n[메시지 작성]\n내용을 입력하세요: ";
    std::string content;
    std::getline(std::cin, content);
    if (content.empty())
        return;

    int success_cnt = 0;
    for (size_t i = 0; i < users.size(); i++)
    {
        if (selected[i])
        {
            json msg_req = make_request(PKT_MSG_SEND_REQ);
            msg_req["payload"] = {
                {"to", users[i]["email"].get<std::string>()},
                {"content", content}};
            send_json(sock, msg_req);
            json msg_res;
            recv_json(sock, msg_res);
            if (msg_res["code"] == VALUE_SUCCESS)
                success_cnt++;
        }
    }
    std::cout << "\n총 " << success_cnt << "명의 유저에게 메시지를 발송했습니다.\n계속하려면 Enter...";
    std::cin.get();
}

void handle_admin_menu(int sock)
{
    while (true)
    {
        int choice = tui_menu("관리자 모드", {"접속 유저 목록",
                                              "모든 유저에게 메시지 보내기",
                                              "비활성화된 유저 목록",
                                              "뒤로 가기"});

        if (choice == -1 || choice == 3)
            break;
        if (choice == 0)
            admin_show_user_list(sock, false);
        if (choice == 1)
            admin_broadcast_message(sock);
        if (choice == 2)
            admin_show_user_list(sock, true);
    }
}