// ============================================================================ 
// 파일명: client_main.cpp                                                       // 파일명
// 목적: UI는 유지 + Length-Prefix 통신 + 기능은 핸들러로만 호출하는 실행부 뼈대  // 목적
// ============================================================================ 

#include <iostream>                                                                 
#include <string>                                                                   
#include <limits>                                                                   
#include <arpa/inet.h>                                                              
#include <unistd.h>                                                                 
#include <nlohmann/json.hpp>                                                        
#include "./protocol/json_packet.hpp"                                                   
#include "./protocol/protocal.h"
#include "./protocol/protocol_schema.h"
#include "message_client_handle.hpp"

extern "C" {                                // C 모듈을 C 링크로 사용
#include "./protocol/packet.h"           // length-prefix send/recv 공용 모듈
}                                           // extern "C" 끝

using json = nlohmann::json;                                                       

static const char* SERVER_IP   = "127.0.0.1";                                       // 서버 IP
static const int   SERVER_PORT = 5010;                                              

// ============================================================================ // 콘솔 입력 버퍼 정리 유틸
// ============================================================================ 
static void clear_stdin_line()                                                      // cin 잔여 입력 제거 함수
{                                                                                   // 함수 시작
    std::cin.clear();                                                               // 입력 스트림 오류 상태 초기화
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');             // 한 줄 끝까지 버림
}                                                                                   // 함수 끝

// ============================================================================ // 서버 연결 생성
static int connect_server_or_die()                                                  // 서버 연결 소켓 생성 함수
{                                                                                   // 함수 시작
    int sock =:socket(PF_INET, SOCK_STREAM, 0);                                   // TCP 소켓 생성
    if (sock < 0)                                                                   // 생성 실패 체크
    {                                                                               
        std::cerr << "소켓 생성 실패\n";                                            // 에러 출력
        return -1;                                                                  // 실패 반환
    }                                                                               

    sockaddr_in serv{};                                                             // 서버 주소 구조체
    serv.sin_family = AF_INET;                                                      // IPv4
    serv.sin_port = htons(SERVER_PORT);                                             // 포트 네트워크 바이트 오더 변환

    if (inet_pton(AF_INET, SERVER_IP, &serv.sin_addr) != 1)                         // IP 변환 실패 체크
    {                                                                               
        std::cerr << "IP 변환 실패\n";                                              // 에러 출력
       :close(sock);                                                              // 소켓 닫기
        return -1;                                                                  // 실패 반환
    }                                                                               

    if (::connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0)                        // 서버 연결
    {                                                                               
        std::cerr << "서버 연결 실패\n";                                            // 에러 출력
       :close(sock);                                                              // 소켓 닫기
        return -1;                                                                  // 실패 반환
    }                                                                               
    std::cout << "===============================================================\n"; 
    std::cout << " 서버에 연결되었습니다.\n";                                      
    std::cout << "===============================================================\n"; 
    return sock;                                                                    
}                                                                                 
// ============================================================================ 
int main()                                                                          // main 시작
{                                                                                   // 블록 시작
    int sock = connect_server_or_die();                                             // 서버 연결(1회 연결 유지)
    if (sock < 0)                                                                   
    {                                                                               
        return 1;                                                                   
    }                                                                               

    bool running  = true;                                                           
    bool logged_in = false;                                                         // 로그인 상태 플래그

    // ===================================================================== // 메인 루프
    while (running)                                                                 // 
    {                                                                               
        //회원가입 루프(로그인 전 화면)
        while (running && !logged_in)                                               // 로그인 전에는 이 메뉴만 반복
        {                                                                           // while 시작
            int choice = -1;                                                        // 메뉴 선택 변수

            system("clear");                                                        // 화면 정리(리눅스)
            std::cout << "==========================\n";                            
            std::cout << "   3LOUD    \n";                                           // UI 타이틀
            std::cout << "==========================\n";                            
            std::cout << "1. 로그인\n";                                              // UI 메뉴
            std::cout << "2. 회원가입\n";                                            // UI 메뉴
            std::cout << "0. 종료\n";                                                // UI 메뉴
            std::cout << ">> ";                                                     

            if (!(std::cin >> choice))                                              // 숫자 입력 실패
            {                                                                       
                clear_stdin_line();                                                 // 입력 라인 정리
                continue;                                                           // 다시 메뉴
            }                                                                       
            clear_stdin_line();                                                     // 남은 개행 제거

            if (choice == 0)                                                        // 종료 선택
            {                                                                       
                running = false;                                                    // 전체 종료 
                break;                                                             
            }                                                                       

            if (choice == 1)                                                        // 로그인 선택
            {                                                                       
                // --------------------------------------------------------- // 로그인은 팀원 핸들러가 구현
                // 규칙: 성공하면 true 반환, 실패면 false 반환                
                logged_in = handle_login(sock);                                     // 로그인 핸들러 호출
                // --------------------------------------------------------- // 로그인 성공이면 아래 메인메뉴로 넘어감
                continue;                                                           // 메뉴 루프 진행
            }                                                                       

            if (choice == 2)                                                        // 회원가입 선택
            {                                                                       
                // --------------------------------------------------------- // 회원가입은 "가입만" 하고 로그인 화면 유지
                handle_signup(sock);                                                // 회원가입 핸들러 호출
                // --------------------------------------------------------- // 가입 후에도 logged_in은 false 유지
                continue;                                                           // 다시 로그인/회원가입 메뉴로
            }                                                                       
        }                                                                           
        if (!running)                                                               // 종료 
        {                                                                           
            break;                                                                 
        }                                                                           
        // ================================================================= // 2) 로그인 후 메인 메뉴 루프
        while (running && logged_in)                                                // 로그인 상태에서만 반복
        {                                                                           // while 시작
            int choice = -1;                                                        // 메뉴 선택 변수

            system("clear");                                                        // 화면 정리
            std::cout << "============================\n";                          
            std::cout << "        메뉴\n";                                          
            std::cout << "============================\n";                          
            std::cout << "1. 파일\n";                                               
            std::cout << "2. 메시지\n";                                              // 메시지 메뉴
            std::cout << "3. 개인 설정\n";                                           // 개인설정 메뉴
            std::cout << "4. 로그 아웃\n";                                           // 로그아웃
            std::cout << "5. 프로그램 종료\n\n";                                     // 프로그램 종료
            std::cout << "[입력] ";                                                // 입력 프롬프트

            if (!(std::cin >> choice))                                              // 입력 실패 처리
            {                                                                       
                clear_stdin_line();                                                 // 입력 라인 정리
                continue;                                                           // 다시 메뉴
            }                                                                       
            clear_stdin_line();                                                     // 남은 개행 제거

            if (choice == 4)                                                        // 로그아웃
            {                                                                       
                handle_logout(sock);                                                
                logged_in = false;                                                  // 로그인 상태 해제
                break;                                                              
            }                                                                       
            if (choice == 5)                             
            {                                                                       
                running = false;                            
                break;                                                              
            }                                                                       

            // ================================================================= // 파일 메뉴
            if (choice == 1)                                                        // 파일 메뉴 선택
            {                                                                       
                // ------------------------------------------------------------ // 파일 메뉴도 "UI 유지 + 핸들러 호출"만
                bool back = false;                                                  // 뒤로가기 플래그
                while (!back && running && logged_in)                               // 뒤로가기 전까지 반복
                {                                                                   // while 시작
                    int sub = -1;                                                   // 파일 서브메뉴 선택 변수

                    system("clear");                                                // 화면 정리
                    std::cout << "============================\n";                  
                    std::cout << "        파일 메뉴\n";                             // UI 제목
                    std::cout << "============================\n";                  
                    std::cout << "1. 파일 목록\n";                                  // 파일 목록
                    std::cout << "2. 파일 업로드\n";                                // 파일 업로드
                    std::cout << "3. 파일 다운로드\n";                              // 파일 다운로드
                    std::cout << "4. 뒤로가기\n";                                   // 뒤로가기
                    std::cout << "[입력] ";                                       // 입력 프롬프트

                    if (!(std::cin >> sub))                                         // 입력 실패 처리
                    {                                                               
                        clear_stdin_line();                                         // 입력 정리
                        continue;                                                   // 다시 서브메뉴
                    }                                                               
                    clear_stdin_line();                                             // 개행 제거

                    if (sub == 4)                                                   // 뒤로가기
                    {                                                               
                        back = true;                                                // 루프 종료
                        continue;                                                   // 상위 메뉴로
                    }                                                               

                    if (sub == 1)                                                   // 파일 목록
                    {                                                               
                        handle_file_list(sock);                                     // 파일 목록 핸들러 호출
                        continue;                                                   // 서브메뉴로 복귀
                    }                                                               

                    if (sub == 2)                                                   // 파일 업로드
                    {                                                               
                        handle_file_upload(sock);                                   // 업로드 핸들러 호출
                        continue;                                                   // 서브메뉴로 복귀
                    }                                                               

                    if (sub == 3)                                                   // 파일 다운로드
                    {                                                               
                        handle_file_download(sock);                                 // 다운로드 핸들러 호출
                        continue;                                                   // 서브메뉴로 복귀
                    }                                                               
                }                                                                   // 파일 서브메뉴 루프 끝
                continue;                                                           
            }                                                                        

            // ================================================================= // 메시지 메뉴
            if (choice == 2)                                                        // 메시지 메뉴 선택
            {                                                                       
                handle_message_menu(sock);                                          // 메시지 메뉴 핸들러(내부 서브메뉴 포함 가능)
                continue;                                                           // 메인 메뉴로 복귀
            }                                                                       

            // ================================================================= // 개인 설정 메뉴
            if (choice == 3)                                                        // 개인 설정 메뉴 선택
            {                                                                       
                handle_profile_menu(sock);                                          // 개인설정 핸들러(닉변 등)
                continue;                                                           // 메인 메뉴로 복귀
            }                                                                       
        }                                                                           // 로그인 후 메인 메뉴 루프 끝
    }                                                                               // 메인 루프 끝

   :close(sock);                                                                  // 소켓 종료
    return 0;                                                                       // 종료 코드
}                                                                                   // main 끝