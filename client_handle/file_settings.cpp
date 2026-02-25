// ============================================================================
// 파일명: file_settings.cpp
// 목적: 요구사항 13-3 "파일설정" 메뉴 전체 구현
//
// 13-3-1. 클라우드 용량 확인
// 13-3-2. 파일 크기 제한 설정
// 13-3-3. 받는 위치 설정
// 13-3-4. 폴더 관리 (생성 / 삭제, 내부 파일 있으면 삭제 불가)
// ============================================================================

#include "file_settings.hpp"
#include "tui.hpp"
#include "file_client.hpp"               // g_user_no
#include "../client/client_net.hpp"      // send_json / recv_json
#include "../protocol/json_packet.hpp"   // make_request
#include "../protocol/protocal.h"        // PKT_SETTINGS_*, PKT_FILE_LIST_REQ

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>                       // getenv
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 파일 크기를 사람이 읽기 좋은 단위로 변환
// ─────────────────────────────────────────────────────────────────
static std::string human_size(int64_t b)
{
    if (b <= 0)              return "제한 없음";                        // 0이하이면 제한 없음
    if (b < 1024)            return std::to_string(b) + " B";          // 바이트 단위
    if (b < 1024*1024)       return std::to_string(b/1024) + " KB";    // 킬로바이트 단위
    if (b < 1024*1024*1024LL)return std::to_string(b/(1024*1024))+" MB"; // 메가바이트 단위
    return std::to_string(b/(1024*1024*1024LL)) + " GB";               // 기가바이트 단위
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 기본 다운로드 폴더 경로 반환 (~/Downloads)
// ─────────────────────────────────────────────────────────────────
static std::string default_download_dir()
{
    const char* home = getenv("HOME");             // 환경변수 HOME 읽기
    std::string h    = home ? home : "/tmp";       // HOME 없으면 /tmp 대체
    return h + "/Downloads";                       // ~/Downloads 반환
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 로컬 설정 파일 경로 반환 (~/.3loud_settings.json)
// ─────────────────────────────────────────────────────────────────
static std::string settings_path()
{
    const char* home = getenv("HOME");             // HOME 환경변수 읽기
    std::string h    = home ? home : "/tmp";       // HOME 없으면 /tmp 대체
    return h + "/.3loud_settings.json";            // 숨김 파일로 홈에 저장
}

// ─────────────────────────────────────────────────────────────────
//  설정 로드: 파일이 없거나 파싱 실패 시 기본값 반환
// ─────────────────────────────────────────────────────────────────
FileSettings load_file_settings()
{
    FileSettings s;
    s.max_recv_size = 0;                           // 기본값: 파일 크기 제한 없음
    s.download_dir  = "";                          // 기본값: 비어있으면 get_download_dir에서 기본 폴더 사용

    std::ifstream ifs(settings_path());            // 설정 파일 열기
    if (!ifs.is_open()) return s;                  // 파일 없으면 기본값 그대로 반환

    try {
        json j;
        ifs >> j;                                  // JSON 파싱
        s.max_recv_size = j.value("max_recv_size", (int64_t)0); // 파일 크기 제한 읽기
        s.download_dir  = j.value("download_dir",  std::string("")); // 저장 폴더 읽기
    } catch (...) {}                               // 파싱 오류 시 기본값 유지

    return s;
}

// ─────────────────────────────────────────────────────────────────
//  설정 저장: JSON 형태로 파일에 기록
// ─────────────────────────────────────────────────────────────────
void save_file_settings(const FileSettings& s)
{
    json j;
    j["max_recv_size"] = s.max_recv_size;          // 파일 크기 제한 저장
    j["download_dir"]  = s.download_dir;           // 저장 폴더 경로 저장

    std::ofstream ofs(settings_path());            // 설정 파일 쓰기 모드로 열기
    if (ofs.is_open()) ofs << j.dump(4);           // 들여쓰기 4칸으로 JSON 저장
}

// ─────────────────────────────────────────────────────────────────
//  유효한 다운로드 폴더 반환
//  설정된 경로가 없거나 폴더가 삭제됐으면 기본 Downloads 폴더 반환
//  요구사항 13-3-3-1: "받는 위치가 사라졌을 경우 기본 위치에 저장"
// ─────────────────────────────────────────────────────────────────
std::string get_download_dir(const FileSettings& s)
{
    if (!s.download_dir.empty() && fs::is_directory(s.download_dir)) {
        return s.download_dir;                     // 설정된 폴더가 실제로 존재하면 그대로 사용
    }
    // 설정 폴더가 없거나 경로가 비어있으면 기본 폴더 사용 (요구사항 13-3-3-1)
    std::string def = default_download_dir();
    fs::create_directories(def);                   // 기본 폴더가 없으면 생성
    return def;
}

// ─────────────────────────────────────────────────────────────────
//  13-3-1: 클라우드 용량 확인
//  서버에 파일 목록 요청(PKT_FILE_LIST_REQ)을 보내
//  응답 payload 의 storage_used / storage_total 을 화면에 표시
// ─────────────────────────────────────────────────────────────────
static void show_storage_info(int sock)
{
    // 파일 목록 요청으로 용량 정보를 함께 받아옴
    json req = make_request(PKT_FILE_LIST_REQ);
    req["user_no"]           = g_user_no;          // 유저 번호 설정
    req["payload"]["folder"] = "";                 // 폴더 필터 없음

    if (!send_json(sock, req)) {                   // 서버에 요청 전송
        tui_detail::clear();
        std::cout << "[오류] 서버 요청 실패\n";
        std::cout << "계속하려면 Enter..."; std::cin.get();
        return;
    }

    json resp;
    if (!recv_json(sock, resp) || resp.value("code", -1) != VALUE_SUCCESS) {
        tui_detail::clear();
        std::cout << "[오류] 서버 응답 실패\n";
        std::cout << "계속하려면 Enter..."; std::cin.get();
        return;
    }

    int64_t used  = resp["payload"].value("storage_used",  (int64_t)0); // 현재 사용 용량
    int64_t total = resp["payload"].value("storage_total", (int64_t)0); // 등급 총 허용 용량
    int64_t free_ = total - used;                   // 남은 용량 계산
    int     pct   = (total > 0) ? (int)(used * 100 / total) : 0; // 사용률(%)

    // 용량 바 시각화 (20칸 기준)
    int bar_fill = (total > 0) ? (int)(used * 20 / total) : 0; // 채워진 칸 수
    std::string bar = "[";
    for (int i = 0; i < 20; i++)
        bar += (i < bar_fill) ? "█" : "░";         // 사용분은 ▓, 빈 공간은 ░
    bar += "]";

    tui_detail::clear();
    tui_detail::print_divider('=');
    printf("  클라우드 용량 확인\n");
    tui_detail::print_divider('=');
    printf("  사용 중  : %s\n",  human_size(used).c_str());   // 사용 중인 용량
    printf("  전체 용량: %s\n",  human_size(total).c_str());  // 등급 총 용량
    printf("  남은 용량: %s\n",  human_size(free_).c_str());  // 남은 용량
    printf("  사용률   : %d%%\n", pct);                        // 사용률 %
    printf("  %s %d%%\n", bar.c_str(), pct);                   // 시각적 바
    tui_detail::print_divider('-');
    printf("  계속하려면 Enter...");
    fflush(stdout);
    std::cin.get();
}

// ─────────────────────────────────────────────────────────────────
//  13-3-2: 파일 크기 제한 설정
//  로컬 설정에 저장, 업로드 시 file_client.cpp 에서 검사
//  0 설정 시 = 클라우드 용량만 허용한다면 제한 없음
// ─────────────────────────────────────────────────────────────────
static void set_max_recv_size(FileSettings& s)
{
    tui_detail::clear();
    tui_detail::print_divider('=');
    printf("  파일 크기 제한 설정\n");
    tui_detail::print_divider('=');
    printf("  현재 설정: %s\n", human_size(s.max_recv_size).c_str()); // 현재 설정값 표시
    printf("\n");
    printf("  받을 파일의 최대 크기를 설정합니다.\n");
    printf("  (0 입력 = 제한 없음, 단위: MB)\n");
    printf("  예) 100 입력 → 100MB 이하 파일만 받음\n");
    tui_detail::print_divider('-');
    printf("  크기 입력 (MB, 0=제한없음, -1=취소): ");
    fflush(stdout);

    // raw 모드 해제 후 입력 받기
    int64_t input_mb = -1;
    if (!(std::cin >> input_mb)) {                 // 숫자 입력 받기
        std::cin.clear();
        std::cin.ignore(4096, '\n');
        return;                                    // 입력 실패 시 취소
    }
    std::cin.ignore(4096, '\n');

    if (input_mb < 0) return;                      // -1 이하: 취소

    s.max_recv_size = input_mb * 1024 * 1024LL;    // MB → bytes 변환
    save_file_settings(s);                         // 로컬 설정 파일에 저장

    tui_detail::clear();
    if (input_mb == 0)
        printf("  [완료] 파일 크기 제한을 해제했습니다.\n");
    else
        printf("  [완료] %lld MB 이하 파일만 받도록 설정했습니다.\n", (long long)input_mb);
    printf("  계속하려면 Enter..."); fflush(stdout);
    std::cin.get();
}

// ─────────────────────────────────────────────────────────────────
//  13-3-3: 받는 위치 설정
//  tui_browse_dir 로 폴더를 탐색해서 선택하면 로컬 설정에 저장
// ─────────────────────────────────────────────────────────────────
static void set_download_dir(FileSettings& s)
{
    tui_detail::clear();
    tui_detail::print_divider('=');
    printf("  받는 위치 설정\n");
    tui_detail::print_divider('=');
    printf("  현재 설정: %s\n",
           s.download_dir.empty() ? "(기본값)" : s.download_dir.c_str()); // 현재 저장 위치 표시
    printf("\n  폴더 탐색기를 시작합니다. Enter 를 누르세요...\n");
    fflush(stdout);
    std::cin.get();

    // tui 방향키 폴더 탐색기로 저장 폴더 선택
    std::string start = get_download_dir(s);       // 탐색 시작점 = 현재 설정 폴더 (또는 기본값)
    std::string chosen = tui_browse_dir(start);    // 방향키 폴더 탐색기 실행

    tui_detail::clear();
    if (chosen.empty()) {                          // ESC 로 취소한 경우
        printf("  취소되었습니다. 기존 설정을 유지합니다.\n");
    } else {
        s.download_dir = chosen;                   // 선택한 폴더 경로를 설정에 저장
        save_file_settings(s);                     // 로컬 설정 파일에 저장
        printf("  [완료] 받는 위치가 변경되었습니다:\n");
        printf("  %s\n", chosen.c_str());          // 변경된 경로 출력
    }
    printf("  계속하려면 Enter..."); fflush(stdout);
    std::cin.get();
}

// ─────────────────────────────────────────────────────────────────
//  13-3-4 내부: 클라우드 폴더 목록 조회
//  파일 목록에서 폴더를 추론하는 방식은 파일이 없는 빈 폴더를 감지 못함.
//  → 서버에 PKT_SETTINGS_SET_REQ action="list_folders" 로 직접 요청
// ─────────────────────────────────────────────────────────────────
static std::vector<std::string> fetch_cloud_folders(int sock)
{
    std::vector<std::string> folders;

    json req = make_request(PKT_SETTINGS_SET_REQ);
    req["user_no"]               = g_user_no;
    req["payload"]["action"]     = "list_folders";  // 폴더 목록 조회 액션
    req["payload"]["folder"]     = "";               // 빈 문자열 (필수 필드 충족용)

    if (!send_json(sock, req)) return folders;

    json resp;
    if (!recv_json(sock, resp) || resp.value("code", -1) != VALUE_SUCCESS) return folders;

    json arr = resp["payload"].value("folders", json::array());
    for (auto& f : arr) {
        if (f.is_string()) folders.push_back(f.get<std::string>());
    }
    return folders;
}

// ─────────────────────────────────────────────────────────────────
//  13-3-4: 폴더 관리 - 생성
//  서버에 PKT_SETTINGS_SET_REQ 로 폴더 생성 요청
// ─────────────────────────────────────────────────────────────────
static void create_cloud_folder(int sock)
{
    // ── tui_menu 가 raw 모드를 종료했으므로 여기선 일반 cooked 모드 상태 ──
    // getline 으로 폴더 이름을 입력받을 때 raw 모드가 켜져 있으면
    // 엔터가 인식되지 않아 타입 오류가 발생함 → 이 함수 진입 시 반드시
    // restore_raw 가 먼저 호출된 상태여야 함 (tui_menu 종료 시 자동 처리됨)
    tui_detail::clear();
    tui_detail::show_cursor();                     // 커서 표시 (입력 받을 때 필요)
    tui_detail::print_divider('=');
    printf("  클라우드 폴더 생성\n");
    tui_detail::print_divider('=');
    printf("  올바른 폴더 이름 형식:\n");
    printf("    영문/숫자/언더스코어(_) 만 허용\n");
    printf("    예시: work  /  backup_2024  /  project1\n");
    printf("    금지: my folder  /  path/sub  /  .hidden\n");
    tui_detail::print_divider('-');
    printf("  폴더 이름 입력 (빈 칸 Enter = 취소): ");
    fflush(stdout);

    // 입력 버퍼에 남은 내용 제거 후 getline 으로 한 줄 읽기
    // (tui_menu 종료 직후 Enter 잔여 입력이 남아있을 수 있음)
    std::string name;
    if (std::cin.peek() == '\n') std::cin.ignore(); // 잔여 \n 제거
    std::getline(std::cin, name);

    if (name.empty()) {                            // 빈 입력이면 취소
        tui_detail::clear();
        printf("  취소되었습니다.\n");
        printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
        return;
    }

    // 이름 유효성 검사: 영문/숫자/언더스코어만 허용
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  (c == '_');
        if (!ok) {
            tui_detail::clear();
            printf("  [오류] '%c' 는 사용할 수 없는 문자입니다.\n", c);
            printf("  영문, 숫자, 언더스코어(_) 만 사용 가능합니다.\n");
            printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
            return;
        }
    }

    // 서버에 폴더 생성 요청 (PKT_SETTINGS_SET_REQ)
    json req = make_request(PKT_SETTINGS_SET_REQ);
    req["user_no"]               = g_user_no;
    req["payload"]["action"]     = "create_folder"; // 폴더 생성 액션
    req["payload"]["folder"]     = name;            // 생성할 폴더 이름

    tui_detail::clear();
    if (!send_json(sock, req)) {
        printf("  [오류] 서버 요청 실패\n");
        printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
        return;
    }

    json resp;
    if (!recv_json(sock, resp)) {
        printf("  [오류] 서버 응답 수신 실패\n");
        printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
        return;
    }

    if (resp.value("code", -1) == VALUE_SUCCESS) {
        printf("  [완료] 폴더 '%s' 가 생성되었습니다.\n", name.c_str()); // 생성 성공
    } else {
        printf("  [오류] %s\n", resp.value("msg", std::string("폴더 생성 실패")).c_str()); // 서버 오류 메시지
    }
    printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
}

// ─────────────────────────────────────────────────────────────────
//  13-3-4: 폴더 관리 - 삭제
//  내부에 파일이 있으면 삭제 불가 (요구사항 13-3-4)
// ─────────────────────────────────────────────────────────────────
static void delete_cloud_folder(int sock)
{
    // 기존 폴더 목록 조회
    auto folders = fetch_cloud_folders(sock);

    if (folders.empty()) {                         // 삭제할 폴더가 없는 경우
        tui_detail::clear();
        printf("  삭제할 클라우드 폴더가 없습니다.\n");
        printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
        return;
    }

    // 방향키로 폴더 선택
    std::vector<std::string> items;
    for (auto& f : folders) items.push_back("[폴더] " + f); // 목록에 폴더명 추가
    items.push_back("취소");                        // 마지막에 취소 항목 추가

    int choice = tui_menu("삭제할 폴더 선택", items); // 방향키 메뉴로 선택
    if (choice < 0 || choice == (int)items.size() - 1) return; // ESC 또는 취소 선택

    std::string target = folders[choice];          // 삭제할 폴더 이름

    // 삭제 최종 확인
    int confirm = tui_menu("'" + target + "' 폴더를 삭제하시겠습니까?\n"
                           "  (내부에 파일이 있으면 삭제 불가)",
                           {"아니오 (취소)", "예 (삭제)"});
    if (confirm != 1) return;                      // "예" 가 아니면 취소

    // 서버에 폴더 삭제 요청
    json req = make_request(PKT_SETTINGS_SET_REQ);
    req["user_no"]               = g_user_no;
    req["payload"]["action"]     = "delete_folder"; // 폴더 삭제 액션
    req["payload"]["folder"]     = target;          // 삭제할 폴더 이름

    if (!send_json(sock, req)) {
        tui_detail::clear();
        printf("  [오류] 서버 요청 실패\n");
        printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
        return;
    }

    json resp;
    if (!recv_json(sock, resp)) {
        tui_detail::clear();
        printf("  [오류] 서버 응답 수신 실패\n");
        printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
        return;
    }

    tui_detail::clear();
    if (resp.value("code", -1) == VALUE_SUCCESS) {
        printf("  [완료] 폴더 '%s' 가 삭제되었습니다.\n", target.c_str()); // 삭제 성공
    } else {
        printf("  [오류] %s\n", resp.value("msg", std::string("폴더 삭제 실패")).c_str()); // 오류 (파일 있어서 불가 등)
    }
    printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
}

// ─────────────────────────────────────────────────────────────────
//  13-3-4: 폴더 관리 메뉴
// ─────────────────────────────────────────────────────────────────
static void folder_management_menu(int sock)
{
    while (true) {
        int choice = tui_menu("폴더 관리", {
            "클라우드 폴더 목록 보기",  // 0
            "폴더 생성",                // 1
            "폴더 삭제",                // 2
            "뒤로가기"                  // 3
        });

        if (choice < 0 || choice == 3) break;      // ESC 또는 뒤로가기

        if (choice == 0) {
            // 폴더 목록 표시
            auto folders = fetch_cloud_folders(sock);
            tui_detail::clear();
            tui_detail::print_divider('=');
            printf("  클라우드 폴더 목록\n");
            tui_detail::print_divider('=');
            if (folders.empty()) {
                printf("  (폴더 없음)\n");          // 폴더가 없는 경우
            } else {
                for (size_t i = 0; i < folders.size(); i++)
                    printf("  [%zu] /%s\n", i+1, folders[i].c_str()); // 폴더 목록 출력
            }
            tui_detail::print_divider('-');
            printf("  계속하려면 Enter..."); fflush(stdout); std::cin.get();
        }
        else if (choice == 1) create_cloud_folder(sock); // 폴더 생성
        else if (choice == 2) delete_cloud_folder(sock); // 폴더 삭제
    }
}

// ─────────────────────────────────────────────────────────────────
//  13-3 파일설정 메인 메뉴 (skeleton_client 에서 호출)
// ─────────────────────────────────────────────────────────────────
void handle_file_settings_menu(int sock)
{
    FileSettings s = load_file_settings();         // 로컬 설정 로드

    while (true) {
        // 현재 설정 요약을 메뉴 제목에 표시
        std::string recv_limit = human_size(s.max_recv_size);
        std::string dl_dir     = get_download_dir(s);
        // 경로가 너무 길면 뒷부분만 표시
        if (dl_dir.size() > 30) dl_dir = "..." + dl_dir.substr(dl_dir.size()-28);

        int choice = tui_menu("파일 설정", {
            "클라우드 용량 확인",                            // 13-3-1
            "파일 크기 제한  [현재: " + recv_limit + "]",   // 13-3-2
            "받는 위치 설정  [" + dl_dir + "]",             // 13-3-3
            "폴더 관리",                                     // 13-3-4
            "뒤로가기"
        });

        if (choice < 0 || choice == 4) break;      // ESC 또는 뒤로가기

        if (choice == 0) show_storage_info(sock);  // 13-3-1: 용량 확인
        if (choice == 1) {                         // 13-3-2: 크기 제한
            set_max_recv_size(s);
            s = load_file_settings();              // 저장 후 다시 로드 (표시 갱신)
        }
        if (choice == 2) {                         // 13-3-3: 받는 위치 설정
            set_download_dir(s);
            s = load_file_settings();              // 저장 후 다시 로드
        }
        if (choice == 3) folder_management_menu(sock); // 13-3-4: 폴더 관리
    }
}
