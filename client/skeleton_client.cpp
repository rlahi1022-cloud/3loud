#include <iostream>                         // 표준 입출력
#include <string>                           // 문자열 처리
#include <thread>                           // 스레드
#include <mutex>                            // mutex
#include <atomic>                           // atomic 변수
#include <unistd.h>                         // close()
#include <arpa/inet.h>                      // sockaddr_in
#include <sys/socket.h>                     // socket API
#include <nlohmann/json.hpp>                // JSON 라이브러리

using json = nlohmann::json;                // JSON 별칭

#define SERVER_IP "127.0.0.1"               // 서버 IP
#define SERVER_PORT 9000                    // 서버 포트

int sock_fd;                                // 소켓 FD
std::atomic<bool> running(true);            // 실행 상태
std::mutex send_mutex;                      // send 보호
int global_req_id = 1;                      // 요청 ID 증가값
std::string session_token = "";             // 로그인 후 세션 저장

// =========================
// 클라이언트 상태 정의
// =========================
enum ClientState
{
    STATE_AUTH,                             // 로그인/회원가입 상태
    STATE_MAIN,                             // 메인 메뉴 상태
    STATE_MESSAGE,                          // 메시지 메뉴 상태
    STATE_FILE,                             // 파일 메뉴 상태
    STATE_SETTING,                          // 설정 메뉴 상태
    STATE_EXIT                              // 종료 상태
};

// =========================
// JSON 전송 함수
// =========================
void send_json(json &j)
{
    std::lock_guard<std::mutex> lock(send_mutex);   // 동기화

    std::string data = j.dump();                    // JSON 문자열화
    data += "\n";                                   // 구분용 개행 추가

    send(sock_fd, data.c_str(), data.size(), 0);    // 서버 전송
}

// =========================
// 수신 스레드
// =========================
void recv_thread()
{
    char buffer[4096];                              // 수신 버퍼

    while (running)                                 // 실행 중 반복
    {
        int len = recv(sock_fd, buffer, sizeof(buffer) - 1, 0); // 수신

        if (len <= 0)                               // 종료 또는 오류
        {
            std::cout << "서버 연결 종료\n";       // 출력
            running = false;                        // 종료 플래그
            break;                                  // 루프 탈출
        }

        buffer[len] = '\0';                         // 문자열 종료

        try
        {
            json res = json::parse(buffer);         // JSON 파싱

            int value = res["value"];               // 성공/실패 값

            if (value == 0)                         // 성공
            {
                std::cout << "[성공]\n";           // 성공 출력

                if (res.contains("payload") && res["payload"].contains("session"))
                {
                    session_token = res["payload"]["session"]; // 세션 저장
                }
            }
            else
            {
                std::cout << "[오류 코드] " << value << "\n"; // 오류 출력
            }
        }
        catch (...)
        {
            std::cout << "JSON 파싱 실패\n";       // 예외 처리
        }
    }
}

// =========================
// 회원가입 요청
// =========================
void request_register()
{
    std::string email, pw, name;                   // 입력 변수

    std::cout << "이메일: ";
    std::cin >> email;                             // 입력

    std::cout << "비밀번호: ";
    std::cin >> pw;

    std::cout << "이름: ";
    std::cin >> name;

    json req;
    req["type"] = 0x0001;                          // PKT_AUTH_REGISTER_REQ
    req["req_id"] = global_req_id++;               // 요청 ID 증가
    req["payload"] = {
        {"email", email},
        {"password", pw},
        {"name", name}
    };

    send_json(req);                                // 전송
}

// =========================
// 로그인 요청
// =========================
void request_login()
{
    std::string email, pw;

    std::cout << "이메일: ";
    std::cin >> email;

    std::cout << "비밀번호: ";
    std::cin >> pw;

    json req;
    req["type"] = 0x0003;                          // PKT_AUTH_LOGIN_REQ
    req["req_id"] = global_req_id++;
    req["payload"] = {
        {"email", email},
        {"password", pw}
    };

    send_json(req);
}

// =========================
// 메시지 전송 요청
// =========================
void request_send_message()
{
    if (session_token.empty())                     // 세션 없으면 차단
    {
        std::cout << "로그인 필요\n";
        return;
    }

    std::string receiver, content;

    std::cout << "받는 사람 이메일: ";
    std::cin >> receiver;

    std::cin.ignore();

    std::cout << "메시지 내용: ";
    std::getline(std::cin, content);

    json req;
    req["type"] = 0x0010;                          // PKT_MSG_SEND_REQ
    req["req_id"] = global_req_id++;
    req["payload"] = {
        {"session", session_token},
        {"receiver", receiver},
        {"content", content}
    };

    send_json(req);
}

// =========================
// 메인 함수
// =========================
int main()
{
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);     // 소켓 생성

    sockaddr_in server_addr;                       // 서버 주소
    server_addr.sin_family = AF_INET;              // IPv4
    server_addr.sin_port = htons(SERVER_PORT);     // 포트 설정
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr); // IP 변환

    if (connect(sock_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        std::cout << "서버 연결 실패\n";
        return 1;
    }

    std::cout << "서버 연결 성공\n";

    std::thread t(recv_thread);                    // 수신 스레드 시작

    ClientState state = STATE_AUTH;                // 초기 상태 설정

    while (state != STATE_EXIT && running)         // 상태 기반 루프
    {
        switch (state)
        {
            case STATE_AUTH:
            {
                std::cout << "\n1.회원가입 2.로그인 0.종료\n";
                int choice;
                std::cin >> choice;

                if (choice == 1)
                    request_register();
                else if (choice == 2)
                {
                    request_login();
                    state = STATE_MAIN;            // 로그인 후 메인 이동
                }
                else if (choice == 0)
                    state = STATE_EXIT;

                break;
            }

            case STATE_MAIN:
            {
                std::cout << "\n1.메시지 2.파일 3.설정 4.로그아웃\n";
                int choice;
                std::cin >> choice;

                if (choice == 1)
                    state = STATE_MESSAGE;
                else if (choice == 2)
                    state = STATE_FILE;
                else if (choice == 3)
                    state = STATE_SETTING;
                else if (choice == 4)
                {
                    session_token = "";            // 세션 초기화
                    state = STATE_AUTH;            // 인증 상태로 복귀
                }

                break;
            }

            case STATE_MESSAGE:
            {
                std::cout << "\n1.메시지 보내기 0.뒤로가기\n";
                int choice;
                std::cin >> choice;

                if (choice == 1)
                    request_send_message();
                else if (choice == 0)
                    state = STATE_MAIN;

                break;
            }

            case STATE_FILE:
            {
                std::cout << "파일 기능 구현 예정\n";
                state = STATE_MAIN;                // 임시 복귀
                break;
            }

            case STATE_SETTING:
            {
                std::cout << "설정 기능 구현 예정\n";
                state = STATE_MAIN;                // 임시 복귀
                break;
            }

            default:
                state = STATE_EXIT;                // 예외 종료
        }
    }

    running = false;                               // 종료 플래그
    close(sock_fd);                                // 소켓 닫기
    t.join();                                       // 수신 스레드 종료 대기

    return 0;
}