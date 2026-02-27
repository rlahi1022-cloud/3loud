// ============================================================================
// 파일명: client_main.cpp                                                       // 파일명
// 목적: UI는 유지 + Length-Prefix 통신 + 기능은 핸들러로만 호출하는 실행부 뼈대  // 목적
// ============================================================================
#include <iostream>    // 표준 입출력
#include <string>      // 문자열
#include <limits>      // numeric_limits
#include <arpa/inet.h> // inet_pton, sockaddr_in
#include <unistd.h>    // close
#include <thread>      // 백그라운드 폴링 스레드
#include <atomic>      // g_has_unread 공유 상태
#include <mutex>       // 폴링 소켓 뮤텍스
#include <nlohmann/json.hpp>

using json = nlohmann::json;

extern "C"
{
#include "packet.h"
}
#include "json_packet.hpp"                  // JSON 기본
#include "../client_handle/file_client.hpp" // 팀원들이 구현할 핸들러 선언
#include "../client_handle/tui.hpp"         // 방향키 TUI
#include "protocol_schema.h"                // 스키마
#include "sha256.h"                         // SHA256 해싱
#include "client_net.hpp"                   // 소켓 연결 및 송수신 함수
#include <iomanip>                          // setfill, setw (시간 포맷팅용)
#include <sys/select.h>                     // select()
#include <sys/time.h>                       // timeval
#include <termios.h>                        // 터미널 제어
#include <stdio.h>                          // getchar()
#include <sys/ioctl.h>                      // ioctl()로 터미널 크기 가져오기
#include <sys/types.h>                      // select() 관련
#include <sys/stat.h>                       // 파일 상태 검사
#include <fcntl.h>                          // 파일 제어 옵션
#include <cstring>                          // memset
#include "protocol.h"
#include "client_handlers.h"
#include "client_messagehandler.hpp"
#include "file_settings.hpp"
const char *SERVER_IP = "127.0.0.1"; // 서버 IP(테스트용)
static const int SERVER_PORT = 5011; // 서버 포트(프로젝트 값으로 맞추기)

std::string g_current_user_email;
extern std::string g_msg_prefix;
extern std::string g_msg_suffix;

std::string g_current_pw_hash;  // 폴링 소켓 로그인용

// ── 실시간 폴링 (요구사항 9) ──
std::atomic<bool>  g_has_unread{false};   // 스레드 간 공유 unread 플래그
static int         g_poll_sock = -1;       // 폴링 전용 소켓
static std::mutex  g_poll_mutex;           // 폴링 소켓 뮤텍스
static std::atomic<bool> g_poll_running{false}; // 폴링 스레드 실행 플래그
static std::thread g_poll_thread;          // 폴링 스레드

// ============================================================================ // 콘솔 입력 버퍼 정리 유틸
// ============================================================================ 

// ============================================================================
// 실시간 메시지 폴링 (요구사항 9, 9-1, 9-2)
// - 전용 소켓(g_poll_sock)으로 5초마다 PKT_MSG_LIST_REQ 전송
// - 응답의 has_unread를 g_has_unread(atomic)에 저장
// ============================================================================
static int make_poll_connection()
{
    int s = ::socket(PF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serv.sin_addr) != 1)
        { close(s); return -1; }
    if (::connect(s, (sockaddr*)&serv, sizeof(serv)) < 0)
        { close(s); return -1; }
    return s;
}

static void poll_loop()
{
    while (g_poll_running.load())
    {
        // 5초 대기 (0.1초씩 쪼개서 종료 신호 빠르게 감지)
        for (int i = 0; i < 50 && g_poll_running.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!g_poll_running.load()) break;

        std::lock_guard<std::mutex> lk(g_poll_mutex);
        if (g_poll_sock < 0) continue;

        // PKT_MSG_POLL_REQ 전송 (세션 없이 email+pw_hash 인증, has_unread 확인)
        json req = make_request(PKT_MSG_POLL_REQ);
        req["payload"]["email"]   = g_current_user_email;
        req["payload"]["pw_hash"] = g_current_pw_hash;
        std::string s = req.dump();

        if (packet_send(g_poll_sock, s.c_str(), (uint32_t)s.size()) < 0)
        {
            // 소켓 끊김 → 재연결 시도
            close(g_poll_sock);
            g_poll_sock = make_poll_connection();
            continue;
        }

        char*    rbuf = nullptr;
        uint32_t rlen = 0;
        if (packet_recv(g_poll_sock, &rbuf, &rlen) < 0)
        {
            close(g_poll_sock);
            g_poll_sock = make_poll_connection();
            continue;
        }

        try {
            auto r = json::parse(std::string(rbuf, rlen));
            bool unread = r["payload"].value("has_unread", false);
            g_has_unread.store(unread);
        } catch (...) {}
        free(rbuf);
    }
}

static void start_poll_thread()
{
    // PKT_MSG_POLL_REQ는 세션 없이 email+pw_hash로 직접 인증하므로 로그인 불필요
    g_poll_sock = make_poll_connection();
    g_poll_running.store(true);
    g_poll_thread = std::thread(poll_loop);
}

static void stop_poll_thread()
{
    g_poll_running.store(false);
    if (g_poll_thread.joinable()) g_poll_thread.join();
    if (g_poll_sock >= 0) { close(g_poll_sock); g_poll_sock = -1; }
    g_has_unread.store(false);
}

void clear_stdin_line()                                          // cin 잔여 입력 제거 함수
{                                                                       // 함수 시작
    std::cin.clear();                                                   // 입력 스트림 오류 상태 초기화
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // 한 줄 끝까지 버림
} // 함수 끝

// ============================================================================ // 서버 연결 생성(1회 연결 유지 방식)
// ============================================================================
static int connect_server_or_die()                // 서버 연결 소켓 생성 함수
{                                                 // 함수 시작
    int sock = ::socket(PF_INET, SOCK_STREAM, 0); // TCP 소켓 생성
    if (sock < 0)                                 // 생성 실패 체크
    {                                             // if 시작
        std::cerr << "소켓 생성 실패\n";          // 에러 출력
        return -1;                                // 실패 반환
    }
    sockaddr_in serv{};                 // 서버 주소 구조체
    serv.sin_family = AF_INET;          // IPv4
    serv.sin_port = htons(SERVER_PORT); // 포트 네트워크 바이트 오더 변환

    if (inet_pton(AF_INET, SERVER_IP, &serv.sin_addr) != 1) // IP 변환 실패 체크
    {                                                       // if 시작
        std::cerr << "IP 변환 실패\n";                      // 에러 출력
        close(sock);                                        // 소켓 닫기
        return -1;                                          // 실패 반환
    }

    if (::connect(sock, (sockaddr *)&serv, sizeof(serv)) < 0) // 서버 연결
    {                                                         // if 시작
        std::cerr << "서버 연결 실패\n";                      // 에러 출력
        close(sock);                                          // 소켓 닫기
        return -1;                                            // 실패 반환
    }

    std::cout << "===============================================================\n"; // UI 라인
    std::cout << " 서버에 연결되었습니다.\n";                                         // UI 문구
    std::cout << "===============================================================\n"; // UI 라인

    return sock; // 연결된 소켓 반환
} // 함수 끝

// ============================================================================ // 프로그램 진입점
// ============================================================================

int main()                              // main 시작
{                                       // 블록 시작
    int sock = connect_server_or_die(); // 서버 연결(1회 연결 유지)
    if (sock < 0)                       // 연결 실패면 종료
    {                                   // if 시작
        return 1;                        
    }

    bool running = true;    // 프로그램 실행 플래그
    bool logged_in = false; // 로그인 상태 플래그

    // ===================================================================== // 메인 루프(프로그램 전체)
    while (running) // 프로그램이 실행 중이면 반복
    {               // while 시작

        // ================================================================= // 1) 로그인/회원가입 루프(로그인 전 화면)
        while (running && !logged_in) // 로그인 전에는 이 메뉴만 반복
        {                             // while 시작

            // tui_menu: 0=로그인, 1=회원가입, 2=종료 / ESC=종료
            int choice = tui_menu("3LOUD", {"로그인", "회원가입", "종료"});

            if (choice == -1 || choice == 2) // ESC 또는 종료
            {                                // if 시작
                running = false;             // 전체 종료 플래그 끄기
                break;                       // 로그인 루프 탈출
            }

            if (choice == 0)                    // 로그인 선택
            {                                   // if 시작
                logged_in = handle_login(sock); // 로그인 핸들러 호출
                if (logged_in) {
                    load_receiver_history();  // 수신자 이력 로드
                    start_poll_thread();      // 실시간 폴링 시작 (요구사항 9)
                }
                continue; // 메뉴 루프 진행
            } 


            if (choice == 1)         // 회원가입 선택
            {                        // if 시작
                handle_signup(sock); // 회원가입 핸들러 호출
                continue;            // 다시 로그인/회원가입 메뉴로
            }
        } // 로그인/회원가입 루프 끝

        if (!running) // 종료 선택이면 빠져나감
        {             // if 시작
            break;    // 메인 루프 탈출
        }

        // ================================================================= // 2) 로그인 후 메인 메뉴 루프
        while (running && logged_in) // 로그인 상태에서만 반복
        {                            // while 시작
            // ── 안읽은 메시지 여부 확인 (메인 메뉴 진입마다 1회 요청) ──
            bool has_unread = false;
            {
                json poll_req = make_request(PKT_MSG_LIST_REQ);
                poll_req["payload"]["page"] = 0;
                std::string s = poll_req.dump();

                if (packet_send(sock, s.c_str(), (uint32_t)s.size()) == 0)
                {
                    char *rbuf = nullptr;
                    uint32_t rlen = 0;
                    if (packet_recv(sock, &rbuf, &rlen) == 0)
                    {
                        auto r = json::parse(std::string(rbuf, rlen));
                        has_unread = r["payload"].value("has_unread", false);
                        free(rbuf);
                    }
                }
            }

            // tui_menu: 0=파일, 1=메시지, 2=개인설정, 3=로그아웃, 4=종료
            // items_fn으로 g_has_unread 실시간 반영 (100ms마다 갱신)
            auto main_items_fn = []() -> std::vector<std::string> {
                std::string msg = g_has_unread.load()
                    ? "메시지  \033[33m[!] 읽지 않은 메시지\033[0m"
                    : "메시지";
                return {"파일", msg, "환경 설정", "로그 아웃", "프로그램 종료"};
            };
            int choice = tui_menu("3LOUD 메인 메뉴", main_items_fn(), main_items_fn);

            if (choice == -1 || choice == 4) // ESC 또는 종료
            {                                // if 시작
                running = false;             // 전체 종료 플래그
                break;                       // 메인 메뉴 루프 탈출
            }
            if (choice == 3)         // 로그아웃
            {                        // if 시작
                handle_logout(sock); // 로그아웃 훅
                stop_poll_thread();  // 폴링 중단
                logged_in = false;   // 로그인 상태 해제
                break;               // 메인 메뉴 루프 탈출 -> 로그인 화면으로
            }

            // ================================================================= // 파일 메뉴
            if (choice == 0)                          // 파일 메뉴 선택
            {                                         // if 시작
                bool back = false;                    // 뒤로가기 플래그
                while (!back && running && logged_in) // 뒤로가기 전까지 반복
                {                                     // while 시작
                    int sub = tui_menu("파일 메뉴", {"파일 목록",
                                                     "파일 업로드",
                                                     "파일 다운로드",
                                                     "파일 삭제",
                                                     "뒤로가기"});

                    if (sub == -1 || sub == 4)
                    {
                        back = true;
                        continue;
                    } // ESC or 뒤로가기
                    if (sub == 0)
                    {
                        handle_file_list(sock);
                        continue;
                    }
                    if (sub == 1)
                    {
                        handle_file_upload(sock);
                        continue;
                    }
                    if (sub == 2)
                    {
                        handle_file_download(sock);
                        continue;
                    }
                    if (sub == 3)
                    {
                        handle_file_delete(sock);
                        continue;
                    }
                }
                continue; // 메인 메뉴로 복귀
            } // 파일 메뉴 if 끝

            // ================================================================= // 메시지 메뉴
            if (choice == 1)               // 메시지 메뉴 선택
            {                              // if 시작
                handle_message_menu(sock); // 메시지 메뉴 핸들러
                continue;                  // 메인 메뉴로 복귀
            }

            // ================================================================= // 개인 설정 메뉴
            if (choice == 2)                          // 개인 설정 메뉴 선택
            {                                         // if 시작
                bool back = false;                    // 뒤로가기 플래그
                while (!back && running && logged_in) // 로그인 상태에서 반복
                {
                    int sub = tui_menu("환경설정", {"개인 설정",
                                                    "파일 설정",
                                                    "메시지 설정",
                                                    "뒤로가기"});

                    if (sub == -1 || sub == 3)
                    {
                        back = true;
                        continue;
                    } // ESC 또는 뒤로가기
                    if (sub == 0)
                    {
                        bool keep_login = handle_profile_menu(sock);
                        if (!keep_login)
                        {
                            // [수정] 강제 로그아웃 처리: 로그인 상태 해제
                            logged_in = false;
                            std::cout << ">> [Client] 로그인 화면으로 이동합니다.\n";
                            sleep(1); // 사용자 확인 대기 (선택 사항)
                            break;    // 이제 while(running && logged_in)을 탈출하여 로그인 루프로 이동
                        }
                        continue;
                    }
                    if (sub == 1)
                    {
                        handle_file_settings_menu(sock);
                        continue;
                    }
                    if (sub == 2)
                    {
                        handle_message_settings(sock);
                        continue;
                    }
                }
                continue;
            }
        }
    }
    close(sock); // 소켓 종료
    return 0;    
} 
