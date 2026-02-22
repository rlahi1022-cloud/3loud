// ============================================================================ // 구분선
// 파일명: client_main.cpp                                                       // 파일명
// 목적: UI는 유지 + Length-Prefix 통신 + 기능은 핸들러로만 호출하는 실행부 뼈대  // 목적
// ============================================================================ // 구분선

#include <iostream>                                                                 // 표준 입출력
#include <string>                                                                   // 문자열
#include <limits>                                                                   // numeric_limits
#include <arpa/inet.h>                                                              // inet_pton, sockaddr_in
#include <unistd.h>                                                                 // close
#include <nlohmann/json.hpp>                                                        // nlohmann json

#include "packet.h"                                                                 // C 공용 length-prefix 송수신 모듈
#include "json_packet.hpp"                                                          // JSON 기본 템플릿(기본값/공통필드)
#include "client_handlers.h"                                                        // 팀원들이 구현할 핸들러 선언

using json = nlohmann::json;                                                        // json 별칭

static const char* SERVER_IP   = "127.0.0.1";                                       // 서버 IP(테스트용)
static const int   SERVER_PORT = 5010;                                              // 서버 포트(프로젝트 값으로 맞추기)

// ============================================================================ // 콘솔 입력 버퍼 정리 유틸
// ============================================================================ // 구분선
static void clear_stdin_line()                                                      // cin 잔여 입력 제거 함수
{                                                                                   // 함수 시작
    std::cin.clear();                                                               // 입력 스트림 오류 상태 초기화
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');             // 한 줄 끝까지 버림
}                                                                                   // 함수 끝

// ============================================================================ // 서버 연결 생성(1회 연결 유지 방식)
// ============================================================================ // 구분선
static int connect_server_or_die()                                                  // 서버 연결 소켓 생성 함수
{                                                                                   // 함수 시작
    int sock =:socket(PF_INET, SOCK_STREAM, 0);                                   // TCP 소켓 생성
    if (sock < 0)                                                                   // 생성 실패 체크
    {                                                                               // if 시작
        std::cerr << "소켓 생성 실패\n";                                            // 에러 출력
        return -1;                                                                  // 실패 반환
    }                                                                               // if 끝

    sockaddr_in serv{};                                                             // 서버 주소 구조체
    serv.sin_family = AF_INET;                                                      // IPv4
    serv.sin_port = htons(SERVER_PORT);                                             // 포트 네트워크 바이트 오더 변환

    if (inet_pton(AF_INET, SERVER_IP, &serv.sin_addr) != 1)                         // IP 변환 실패 체크
    {                                                                               // if 시작
        std::cerr << "IP 변환 실패\n";                                              // 에러 출력
       :close(sock);                                                              // 소켓 닫기
        return -1;                                                                  // 실패 반환
    }                                                                               // if 끝

    if (::connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0)                        // 서버 연결
    {                                                                               // if 시작
        std::cerr << "서버 연결 실패\n";                                            // 에러 출력
       :close(sock);                                                              // 소켓 닫기
        return -1;                                                                  // 실패 반환
    }                                                                               // if 끝

    std::cout << "===============================================================\n"; // UI 라인
    std::cout << " 서버에 연결되었습니다.\n";                                       // UI 문구
    std::cout << "===============================================================\n"; // UI 라인

    return sock;                                                                    // 연결된 소켓 반환
}                                                                                   // 함수 끝

// ============================================================================ // 프로그램 진입점
// ============================================================================ // 구분선
int main()                                                                          // main 시작
{                                                                                   // 블록 시작
    int sock = connect_server_or_die();                                             // 서버 연결(1회 연결 유지)
    if (sock < 0)                                                                   // 연결 실패면 종료
    {                                                                               // if 시작
        return 1;                                                                   // 종료 코드 반환
    }                                                                               // if 끝

    bool running  = true;                                                           // 프로그램 실행 플래그
    bool logged_in = false;                                                         // 로그인 상태 플래그

    // ===================================================================== // 메인 루프(프로그램 전체)
    while (running)                                                                 // 프로그램이 실행 중이면 반복
    {                                                                               // while 시작

        // ================================================================= // 1) 로그인/회원가입 루프(로그인 전 화면)
        while (running && !logged_in)                                               // 로그인 전에는 이 메뉴만 반복
        {                                                                           // while 시작
            int choice = -1;                                                        // 메뉴 선택 변수

            system("clear");                                                        // 화면 정리(리눅스)
            std::cout << "==========================\n";                            // UI 라인
            std::cout << "   3LOUD    \n";                                           // UI 타이틀
            std::cout << "==========================\n";                            // UI 라인
            std::cout << "1. 로그인\n";                                              // UI 메뉴
            std::cout << "2. 회원가입\n";                                            // UI 메뉴
            std::cout << "0. 종료\n";                                                // UI 메뉴
            std::cout << ">> ";                                                     // 입력 프롬프트

            if (!(std::cin >> choice))                                              // 숫자 입력 실패(문자 등)
            {                                                                       // if 시작
                clear_stdin_line();                                                 // 입력 라인 정리
                continue;                                                           // 다시 메뉴
            }                                                                       // if 끝
            clear_stdin_line();                                                     // 남은 개행 제거

            if (choice == 0)                                                        // 종료 선택
            {                                                                       // if 시작
                running = false;                                                    // 전체 종료 플래그 끄기
                break;                                                              // 로그인 루프 탈출
            }                                                                       // if 끝

            if (choice == 1)                                                        // 로그인 선택
            {                                                                       // if 시작
                // --------------------------------------------------------- // 로그인은 팀원 핸들러가 구현
                // 규칙: 성공하면 true 반환, 실패면 false 반환                // 계약(인터페이스)
                logged_in = handle_login(sock);                                     // 로그인 핸들러 호출
                // --------------------------------------------------------- // 로그인 성공이면 아래 메인메뉴로 넘어감
                continue;                                                           // 메뉴 루프 진행
            }                                                                       // if 끝

            if (choice == 2)                                                        // 회원가입 선택
            {                                                                       // if 시작
                // --------------------------------------------------------- // 회원가입은 "가입만" 하고 로그인 화면 유지
                handle_signup(sock);                                                // 회원가입 핸들러 호출
                // --------------------------------------------------------- // 가입 후에도 logged_in은 false 유지
                continue;                                                           // 다시 로그인/회원가입 메뉴로
            }                                                                       // if 끝
        }                                                                           // 로그인/회원가입 루프 끝

        if (!running)                                                               // 종료 선택이면 빠져나감
        {                                                                           // if 시작
            break;                                                                  // 메인 루프 탈출
        }                                                                           // if 끝

        // ================================================================= // 2) 로그인 후 메인 메뉴 루프
        while (running && logged_in)                                                // 로그인 상태에서만 반복
        {                                                                           // while 시작
            int choice = -1;                                                        // 메뉴 선택 변수

            system("clear");                                                        // 화면 정리
            std::cout << "============================\n";                          // UI 라인
            std::cout << "        메뉴\n";                                           // UI 제목
            std::cout << "============================\n";                          // UI 라인
            std::cout << "1. 파일\n";                                                // 채팅방 자리 -> 파일 메뉴로 변경
            std::cout << "2. 메시지\n";                                              // 메시지 메뉴
            std::cout << "3. 개인 설정\n";                                           // 개인설정 메뉴
            std::cout << "4. 로그 아웃\n";                                           // 로그아웃
            std::cout << "5. 프로그램 종료\n\n";                                     // 프로그램 종료
            std::cout << "[입력] ";                                                // 입력 프롬프트

            if (!(std::cin >> choice))                                              // 입력 실패 처리
            {                                                                       // if 시작
                clear_stdin_line();                                                 // 입력 라인 정리
                continue;                                                           // 다시 메뉴
            }                                                                       // if 끝
            clear_stdin_line();                                                     // 남은 개행 제거

            if (choice == 4)                                                        // 로그아웃
            {                                                                       // if 시작
                handle_logout(sock);                                                // 로그아웃 훅(필요시 서버 통지)
                logged_in = false;                                                  // 로그인 상태 해제
                break;                                                              // 메인 메뉴 루프 탈출 -> 로그인 화면으로
            }                                                                       // if 끝

            if (choice == 5)                                                        // 프로그램 종료
            {                                                                       // if 시작
                running = false;                                                    // 전체 종료 플래그
                break;                                                              // 메인 메뉴 루프 탈출
            }                                                                       // if 끝

            // ================================================================= // 파일 메뉴
            if (choice == 1)                                                        // 파일 메뉴 선택
            {                                                                       // if 시작
                // ------------------------------------------------------------ // 파일 메뉴도 "UI 유지 + 핸들러 호출"만
                bool back = false;                                                  // 뒤로가기 플래그
                while (!back && running && logged_in)                               // 뒤로가기 전까지 반복
                {                                                                   // while 시작
                    int sub = -1;                                                   // 파일 서브메뉴 선택 변수

                    system("clear");                                                // 화면 정리
                    std::cout << "============================\n";                  // UI 라인
                    std::cout << "        파일 메뉴\n";                             // UI 제목
                    std::cout << "============================\n";                  // UI 라인
                    std::cout << "1. 파일 목록\n";                                  // 파일 목록
                    std::cout << "2. 파일 업로드\n";                                // 파일 업로드
                    std::cout << "3. 파일 다운로드\n";                              // 파일 다운로드
                    std::cout << "4. 뒤로가기\n";                                   // 뒤로가기
                    std::cout << "[입력] ";                                       // 입력 프롬프트

                    if (!(std::cin >> sub))                                         // 입력 실패 처리
                    {                                                               // if 시작
                        clear_stdin_line();                                         // 입력 정리
                        continue;                                                   // 다시 서브메뉴
                    }                                                               // if 끝
                    clear_stdin_line();                                             // 개행 제거

                    if (sub == 4)                                                   // 뒤로가기
                    {                                                               // if 시작
                        back = true;                                                // 루프 종료
                        continue;                                                   // 상위 메뉴로
                    }                                                               // if 끝

                    if (sub == 1)                                                   // 파일 목록
                    {                                                               // if 시작
                        handle_file_list(sock);                                     // 파일 목록 핸들러 호출
                        continue;                                                   // 서브메뉴로 복귀
                    }                                                               // if 끝

                    if (sub == 2)                                                   // 파일 업로드
                    {                                                               // if 시작
                        handle_file_upload(sock);                                   // 업로드 핸들러 호출
                        continue;                                                   // 서브메뉴로 복귀
                    }                                                               // if 끝

                    if (sub == 3)                                                   // 파일 다운로드
                    {                                                               // if 시작
                        handle_file_download(sock);                                 // 다운로드 핸들러 호출
                        continue;                                                   // 서브메뉴로 복귀
                    }                                                               // if 끝
                }                                                                   // 파일 서브메뉴 루프 끝

                continue;                                                           // 메인 메뉴로 복귀
            }                                                                       // 파일 메뉴 if 끝

            // ================================================================= // 메시지 메뉴
            if (choice == 2)                                                        // 메시지 메뉴 선택
            {                                                                       // if 시작
                handle_message_menu(sock);                                          // 메시지 메뉴 핸들러(내부 서브메뉴 포함 가능)
                continue;                                                           // 메인 메뉴로 복귀
            }                                                                       // if 끝

            // ================================================================= // 개인 설정 메뉴
            if (choice == 3)                                                        // 개인 설정 메뉴 선택
            {                                                                       // if 시작
                handle_profile_menu(sock);                                          // 개인설정 핸들러(닉변 등)
                continue;                                                           // 메인 메뉴로 복귀
            }                                                                       // if 끝
        }                                                                           // 로그인 후 메인 메뉴 루프 끝
    }                                                                               // 메인 루프 끝

   :close(sock);                                                                  // 소켓 종료
    return 0;                                                                       // 종료 코드
}                                                                                   // main 끝