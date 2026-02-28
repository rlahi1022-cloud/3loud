// ============================================================================
// 파일명: server.cpp                                                          // 파일명 설명
// 목적: epoll 기반 메인루프 + worker thread + DB(Worker 전용) + length-prefix // 목적 설명
// 전제: common/packet.c, common/packet.h 를 공용 모듈로 사용                  // 전제 설명
// 플랫폼: Linux                                                                // 플랫폼 설명
// ============================================================================
#include "email.h"
#include <iostream>            // 표준 입출력 사용
#include <string>              // std::string 사용
#include <vector>              // std::vector 사용
#include <unordered_map>       // 세션 맵 사용
#include <unordered_set>       // 스트리밍 소켓 집합
#include <queue>               // 큐 사용
#include <mutex>               // mutex 사용
#include <condition_variable>  // condition_variable 사용
#include <thread>              // thread 사용
#include <atomic>              // atomic 사용
#include <cstring>             // memset, memcpy 사용
#include <cerrno>              // errno 사용
#include <csignal>             // signal 사용
#include <unistd.h>            // close, read, write 사용
#include <fcntl.h>             // fcntl 사용
#include <sys/socket.h>        // socket 관련
#include <netinet/in.h>        // sockaddr_in
#include <arpa/inet.h>         // inet_ntop, inet_pton
#include <sys/epoll.h>         // epoll
#include <sys/eventfd.h>       // eventfd
#include <nlohmann/json.hpp>   // JSON 라이브러리 사용
#include <mariadb/conncpp.hpp> // MariaDB C++ Connector 사용
#include <curl/curl.h>         // libcurl 헤더(이메일)
#include <ctime>               // time() 함수 사용을 위해 필요
#include "server.h"
#include "protocol.h"
#include "protocol_schema.h"
#include "message_handler.hpp"
#include "profile_handler.hpp"
#include "blacklisthandler.hpp"
#include "admin_handler.hpp"

extern "C"
{                   // C 모듈을 C 링크로 사용
#include "packet.h" // length-prefix send/recv 공용 모듈
} // extern "C" 끝

using json = nlohmann::json; // json 타입 별칭

#include "../server_handle/file_handler.hpp"
#include "../server_handle/settings_handler.hpp"

static constexpr int EPOLL_MAX_EVENTS = 128;             // epoll 이벤트 배열 크기
static constexpr int MAX_PACKET_SIZE = 10 * 1024 * 1024; // 최대 패킷 크기 제한(10MB)
static constexpr int DEFAULT_PORT = 5012;                // 기본 포트
static constexpr int LISTEN_BACKLOG = 64;                // listen backlog
// [추가] Worker가 Main을 깨우기 위해 사용할 전역 파일 디스크립터
int g_wake_fd = -1;
thread_local int g_current_sock = -1; // 워커 스레드별 현재 처리 소켓 저장
// 전역 맵과 뮤텍스 정의

// 세션 구조체: epoll 스레드에서만 접근/수정하는 것을 기본 원칙으로 둠
// write_buf는 epoll 스레드가 flush 하며, worker는 응답 큐에만 넣음
// ============================================================================

struct Session
{                           // 세션 구조체 시작
    int sock = -1;          // 클라이언트 소켓 fd
    std::string peer_ip;    // 클라이언트 IP 문자열
    uint16_t peer_port = 0; // 클라이언트 포트
    std::string write_buf;  // 전송 대기 버퍼
    std::string read_buf;
}; // 세션 구조체 끝

// ============================================================================
// Task / Response: epoll -> worker / worker -> epoll 교환용
// ============================================================================

struct Task
{                        // 작업 요청 구조체 시작
    int sock = -1;       // 요청이 온 소켓
    std::string payload; // JSON 문자열 payload
}; // 작업 요청 구조체 끝

struct ResponseTask
{                        // 응답 작업 구조체 시작
    int sock = -1;       // 응답 보낼 소켓
    std::string payload; // JSON 문자열 payload
}; // 응답 작업 구조체 끝

// ============================================================================
// 전역(공유) 큐: worker 스레드와 epoll 스레드가 공유하므로 mutex로 보호
// ============================================================================

static std::queue<Task> g_req_q;          // 요청 큐
static std::queue<ResponseTask> g_res_q;  // 응답 큐
static std::mutex g_req_m;                // 요청 큐 mutex
static std::mutex g_res_m;                // 응답 큐 mutex
static std::condition_variable g_req_cv;  // worker를 깨우는 CV
static std::atomic<bool> g_running(true); // 서버 실행 플래그(원자)
// [추가] 접속 중인 유저 관리 (중복 로그인 방지용)
std::unordered_map<std::string, int> g_login_users;  // Email -> Socket
std::unordered_map<int, std::string> g_socket_users; // Socket -> Email (연결 종료 시 빠른 삭제용)
std::mutex g_login_m;                                // 위 맵들을 보호할 Mutex
// [추가] 로그인 실패 횟수 관리
std::map<std::string, int> g_fail_counts; // 이메일 -> 실패횟수
std::mutex g_fail_m;                      // 실패횟수 맵 보호용

// 다운로드 스트리밍 중인 소켓 집합
// worker가 직접 packet_send로 청크를 보내는 동안
// epoll 메인 스레드가 같은 소켓에 write_buf를 flush하는 경쟁을 막음
std::unordered_set<int> g_streaming_socks;
std::mutex              g_streaming_m;

// ============================================================================
// 유틸: non-blocking 설정
// ============================================================================

static bool set_nonblocking(int fd)
{                                      // 논블로킹 설정 함수 시작
    int flags = fcntl(fd, F_GETFL, 0); // 현재 플래그 읽기
    if (flags < 0)
        return false; // 실패 시 false
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false; // O_NONBLOCK 추가
    return true;      // 성공
} // 함수 끝

// 블로킹 모드 전환 (다운로드 청크 직접 전송 시 사용)
static bool set_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) return false;
    return true;
}

// ============================================================================
// 유틸: 안전한 close + 에러 무시
// ============================================================================

static void safe_close(int fd)
{ // 안전 close 함수 시작
    if (fd >= 0)
    {                // 유효 fd인지 확인
        ::close(fd); // close 호출
    }
} // 함수 끝

// ============================================================================
// 유틸: 만료된 인증정보 처리 (메모리 누수 해결)
// ============================================================================
// [청소 함수] 로그를 추가하여 동작 확인
static void cleanup_pending_map()
{
    time_t now = time(NULL);
    std::lock_guard<std::mutex> lock(g_pending_m);

    for (auto it = g_pending_map.begin(); it != g_pending_map.end();)
    {
        double diff = difftime(now, it->second.timestamp);

        // [디버그 출력] 이게 핵심입니다.
        // std::cout << "[DEBUG] 이메일: " << it->first << " | 경과시간: " << diff << "초" << std::endl;

        if (diff > 90)
        { // 90초를 넘기면 삭제
            std::cout << ">> [삭제됨] 인증시간 만료로 삭제: " << it->first << std::endl;
            it = g_pending_map.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// 중복 로그인 방지용:유저 로그인 등록 (성공 시 true, 중복이면 false 반환)
static bool try_login_register(int sock, const std::string &email)
{
    std::lock_guard<std::mutex> lock(g_login_m);

    // 이미 접속 중인 이메일인지 확인
    if (g_login_users.find(email) != g_login_users.end())
    {
        return false; // 중복 로그인
    }

    // 맵에 등록
    g_login_users[email] = sock;
    g_socket_users[sock] = email;
    return true;
}

// 중복 로그인 방지용:유저 로그아웃 처리 (연결 종료 시 호출)
static void logout_unregister(int sock)
{
    std::lock_guard<std::mutex> lock(g_login_m);

    auto it = g_socket_users.find(sock);
    if (it != g_socket_users.end())
    {
        std::string email = it->second;
        g_login_users.erase(email); // 이메일 맵에서 삭제
        g_socket_users.erase(it);   // 소켓 맵에서 삭제
        std::cout << "[Info] User " << email << " 로그아웃 (socket " << sock << " closed).\n";
    }
}

// // ============================================================================
// // 핸들 함수 자리: 팀원들이 이 함수들만 작성하면 됨
// // DB 커넥션은 worker thread 안에서만 사용(요구사항 YES)
// // ============================================================================

// [핸들러] 1단계: 회원가입 요청 (인증번호 발송)
static std::string handle_auth_signup_req(const json &req, sql::Connection &db)
{
    json payload;
    try
    {
        payload = req.at("payload");
    }
    catch (...)
    {
        return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_ERR_INVALID_PACKET, "잘못된 패킷 구조", json::object()).dump();
    }

    std::string email = payload.value("email", "");
    std::string pw = payload.value("pw_hash", "");
    std::string nickname = payload.value("name", "");

    // 1. 입력값 검증
    if (email.empty() || pw.empty() || nickname.empty())
    {
        return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_ERR_INVALID_PACKET, "모든 정보를 입력해주세요.", json::object()).dump();
    }
    if (nickname.length() > 20)
    { // DB 컬럼 크기에 맞춰 제한
        return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_ERR_ID_RULE, "닉네임은 20자 이내여야 합니다.", json::object()).dump();
    }
    if (!isValidEmail(email))
    { // isValidEmail 함수가 있다고 가정
        return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_ERR_ID_RULE, "이메일 형식이 올바르지 않습니다.", json::object()).dump();
    }

    // 2. DB 중복 체크
    try
    {
        // 이메일 중복 확인
        std::unique_ptr<sql::PreparedStatement> st(db.prepareStatement("SELECT 1 FROM users WHERE email = ?"));
        st->setString(1, email);
        std::unique_ptr<sql::ResultSet> res(st->executeQuery());
        if (res->next())
        {
            return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_ERR_ID_DUPLICATE, "이미 가입된 이메일입니다.", json::object()).dump();
        }

        // 닉네임 중복 확인
        std::unique_ptr<sql::PreparedStatement> st_nick(db.prepareStatement("SELECT 1 FROM users WHERE nickname = ?"));
        st_nick->setString(1, nickname);
        std::unique_ptr<sql::ResultSet> res_nick(st_nick->executeQuery());
        if (res_nick->next())
        {
            return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_ERR_NAME_DUPLICATE, "이미 사용 중인 닉네임입니다.", json::object()).dump();
        }
    }
    catch (sql::SQLException &e)
    {
        std::cerr << "[DB Error] Signup Check: " << e.what() << std::endl;
        return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_ERR_DB, "서버 DB 오류입니다.", json::object()).dump();
    }

    // 3. 인증 정보 메모리 저장
    std::string v_code = generate_verification_code(); // 함수 있다고 가정
    // PendingInfo 구조체 정의
    PendingInfo info;
    info.pw = pw;
    info.nickname = nickname;
    info.code = v_code;          // 인증번호
    info.timestamp = time(NULL); // ★ 현재 시간을 확실하게 저장

    {
        std::lock_guard<std::mutex> lock(g_pending_m); // extern 혹은 static 정의 필요
        g_pending_map[email] = info;
    }

    // 4. 메일 발송
    std::cout << "[Auth] Code " << v_code << " generated for " << email << std::endl;
    // email_send 함수 호출 (비동기 권장)
    email_send(email, "[3LOUD] 인증번호 안내", "인증번호: " + v_code);

    return make_resp(PKT_AUTH_REGISTER_REQ, VALUE_SUCCESS, "인증번호가 발송되었습니다.", json::object()).dump();
}

// [핸들러] 2단계: 인증번호 검증 및 가입 완료
static std::string handle_auth_verify_req(const json &req, sql::Connection &db)
{
    json payload;
    try
    {
        payload = req.at("payload");
    }
    catch (...)
    {
        return make_resp(PKT_AUTH_VERIFY_REQ, VALUE_ERR_INVALID_PACKET, "Payload Error", json::object()).dump();
    }

    std::string email = payload.value("email", "");
    std::string code = payload.value("code", "");

    if (email.empty() || code.empty())
    {
        return make_resp(PKT_AUTH_VERIFY_REQ, VALUE_ERR_INVALID_PACKET, "인증번호를 입력해주세요.", json::object()).dump();
    }

    PendingInfo info;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(g_pending_m);
        if (g_pending_map.count(email))
        {
            info = g_pending_map[email];
            found = true;
        }
    }

    // 1. 요청 정보 없음
    if (!found)
    {
        return make_resp(PKT_AUTH_VERIFY_REQ, VALUE_ERR_SESSION, "인증 요청 내역이 없거나 만료되었습니다.", json::object()).dump();
    }

    // 2. 시간 만료 체크 (300초 = 5분)
    time_t now = time(NULL);
    if (now - info.timestamp > 300)
    {
        {
            std::lock_guard<std::mutex> lock(g_pending_m);
            g_pending_map.erase(email);
        }
        return make_resp(PKT_AUTH_VERIFY_REQ, VALUE_ERR_SESSION, "인증 시간이 초과되었습니다. 다시 가입해주세요.", json::object()).dump();
    }

    // 3. 인증번호 불일치
    if (info.code != code)
    {
        return make_resp(PKT_AUTH_VERIFY_REQ, VALUE_ERR_EMAIL_VERIFY, "인증번호가 일치하지 않습니다.", json::object()).dump();
    }

    // 4. DB 저장
    try
    {
        std::unique_ptr<sql::PreparedStatement> st(db.prepareStatement(
            "INSERT INTO users (email, pw_hash, nickname, grade, is_active) VALUES (?, ?, ?, 1, 1)"));
        st->setString(1, email);
        st->setString(2, info.pw);
        st->setString(3, info.nickname);
        st->executeUpdate();

        {
            std::lock_guard<std::mutex> lock(g_pending_m);
            g_pending_map.erase(email);
        }
        // [디버그 출력] 이게 핵심입니다.
        std::cout << "[DEBUG] 회원가입 완료 " << email << std::endl;
        return make_resp(PKT_AUTH_VERIFY_REQ, VALUE_SUCCESS, "회원가입 완료! 로그인해주세요.", json::object()).dump();
    }
    catch (sql::SQLException &e)
    {
        return make_resp(PKT_AUTH_VERIFY_REQ, VALUE_ERR_DB, "계정 생성 중 오류 발생.", json::object()).dump();
    }
}

// [핸들러] 로그인 요청 처리
static std::string handle_auth_login(int client_sock, const json &req, sql::Connection &db)
{
    json payload;
    try
    {
        payload = req.at("payload");
    }
    catch (...)
    {
        return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_INVALID_PACKET, "Payload Error", json::object()).dump();
    }

    std::string email = payload.value("email", "");
    std::string client_pw_hash = payload.value("pw_hash", "");

    if (email.empty() || client_pw_hash.empty())
    {
        return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_INVALID_PACKET, "이메일과 비밀번호를 모두 입력해주세요.", json::object()).dump();
    }

    try
    {
        std::unique_ptr<sql::PreparedStatement> st(db.prepareStatement(
            "SELECT no,pw_hash, nickname, grade, is_active FROM users WHERE email = ?"));
        st->setString(1, email);
        std::unique_ptr<sql::ResultSet> res(st->executeQuery());

        if (res->next())
        {
            // .c_str()을 사용하여 std::string 변환
            int user_no = res->getInt("no"); // 넘버 받아서 로그인 여부 확인
            std::string db_pw_hash = res->getString("pw_hash").c_str();
            std::string nickname = res->getString("nickname").c_str();
            int is_active = res->getInt("is_active");
            int grade = res->getInt("grade");

            // 1. 계정 정지 체크
            if (is_active == 0)
            {
                return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_PERMISSION, "비밀번호 5회 오류로 정지된 계정입니다. 관리자에게 문의하세요.", json::object()).dump();
            }

            // 2. 비밀번호 체크
            if (db_pw_hash == client_pw_hash)
            { // 중복 로그인 체크
                if (!try_login_register(client_sock, email))
                {
                    return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_LOGIN_ID, "이미 접속 중인 계정입니다.", json::object()).dump();
                }
                // 로그인 실패 카운트 초기화(성공한경우)
                {
                    std::lock_guard<std::mutex> lock(g_fail_m);
                    g_fail_counts.erase(email);
                }

                json out_payload;
                out_payload["email"] = email;
                out_payload["nickname"] = nickname;
                out_payload["grade"] = grade;
                out_payload["user_no"] = user_no;

                std::cout << "[Info] User " << email << " 로그인 (socket " << client_sock << " connect).\n";
                return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_SUCCESS, "로그인 성공", out_payload).dump();
            }
            else
            {
                // [실패 시]
                int current_fail = 0;
                {
                    std::lock_guard<std::mutex> lock(g_fail_m);
                    current_fail = ++g_fail_counts[email]; // 카운트 증가
                }

                // 3-3. ★ 5회 도달 시 DB 업데이트 (계정 비활성화)
                if (current_fail >= 5)
                {
                    std::unique_ptr<sql::PreparedStatement> lock_st(db.prepareStatement(
                        "UPDATE users SET is_active = 0 WHERE email = ?"));
                    lock_st->setString(1, email);
                    lock_st->executeUpdate();

                    // 메모리 맵에서도 지워줌 (이미 DB에서 막히므로 관리 불필요)
                    {
                        std::lock_guard<std::mutex> lock(g_fail_m);
                        g_fail_counts.erase(email);
                    }

                    std::cout << ">> [계정 정지] " << email << " (비밀번호 5회 오류)\n";
                    return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_PERMISSION,
                                     "비밀번호 5회 오류로 계정이 비활성화되었습니다.", json::object())
                        .dump();
                }

                // 3-4. 실패 메시지 및 남은 횟수 안내
                std::string msg = "비밀번호가 일치하지 않습니다. 남은 로그인 시도(" + std::to_string(current_fail) + "/5)";
                return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_LOGIN_PW, msg, json::object()).dump();
            }
        }
        else
        {
            return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_LOGIN_ID, "존재하지 않는 계정입니다.", json::object()).dump();
        }
    }
    catch (sql::SQLException &e)
    {
        std::cout << "[DB Error] " << e.what() << std::endl;
        return make_resp(PKT_AUTH_LOGIN_REQ, VALUE_ERR_DB, "DB 조회 중 오류 발생", json::object()).dump();
    }
}

// static std::string handle_msg_send(const json &req, sql::Connection &db)
// {                                                        // 메시지 전송 핸들
//     json payload = req.value("payload", json::object()); // payload
//     std::string to = payload.value("to", "");            // 받는 사람
//     std::string content = payload.value("content", "");  // 내용
//     if (to.empty() || content.empty())
//     {                                                                                            // 필수값 확인
//         return make_resp(VALUE_ERR_INVALID_PACKET, -1, "필수 필드 누락", json::object()).dump(); // 에러
//     }
//     // DB INSERT messages 구현                                                  // 구현 안내
//     return make_resp(VALUE_SUCCESS, 0, "msg_send placeholder", json::object()).dump(); // 임시 성공
// } // 함수 끝

// ============================================================================
// Worker Thread: 요청 처리 담당 (DB 연결은 여기서 생성해서 전용으로 사용)
// ============================================================================

static void worker_loop(std::string db_url, std::string db_user, std::string db_pw)
{                                          // 워커 루프
    sql::Driver *driver = nullptr;         // 드라이버 포인터
    std::unique_ptr<sql::Connection> conn; // DB 커넥션
    try
    {                                                                    // try 시작
        driver = sql::mariadb::get_driver_instance();                    // 드라이버 인스턴스 획득
        sql::Properties props({{"user", db_user}, {"password", db_pw}}); // 접속 속성 생성
        conn.reset(driver->connect(db_url, props));                      // DB 연결 생성
        {                                                                // 블록 시작
            std::unique_ptr<sql::Statement> st(conn->createStatement()); // statement 생성
            st->execute("SET NAMES 'utf8mb4'");                          // 인코딩 설정
        } // 블록 끝
        std::cout << "[Worker] DB connected\n"; // 로그 출력
    }
    catch (const sql::SQLException &e)
    {                                                                    // SQL 예외 처리
        std::cerr << "[Worker] DB connect failed: " << e.what() << "\n"; // 오류 출력
        g_running = false;                                               // 서버 종료 플래그
        return;                                                          // 워커 종료
    } // try-catch 끝

    while (g_running.load())
    {                                                         // 서버 실행 중 반복
        Task task;                                            // 꺼낼 작업
        {                                                     // lock 블록 시작
            std::unique_lock<std::mutex> lk(g_req_m);         // 요청 큐 lock
            g_req_cv.wait(lk, [] {                            // CV 대기
                return !g_req_q.empty() || !g_running.load(); // 큐가 있거나 종료면 깸
            });                                               // wait 끝
            if (!g_running.load())
                break;                  // 종료면 탈출
            task = g_req_q.front();     // 큐 front 복사
            g_req_q.pop();              // 큐 pop
            g_current_sock = task.sock; // ★ 현재 요청 처리 소켓 등록
        } // lock 블록 끝

        std::string out_payload; // 응답 payload 문자열
        int type = 0;

        try
        { // try 시작

            json req = json::parse( // JSON 파싱 (예외 비활성화)
                task.payload,       // 원본 JSON 문자열
                nullptr,            // 콜백 없음
                false               // 예외 던지지 않음
            ); // 파싱 끝

            if (req.is_discarded()) // 파싱 실패 (깨진 JSON / UTF-8 문제 등)
            { // 실패 처리 시작
                out_payload = make_resp(
                    0,                          // type 모름
                    VALUE_ERR_INVALID_PACKET,   // 네 프로젝트 에러 코드
                    "JSON parse failed",
                    json::object()
                ).dump();
            } // 실패 처리 끝
            else
            { // 파싱 성공 시 기존 로직 그대로

                type = req.value("type", 0); // type 방어 파싱

                switch (type)
                { // 기존 switch 그대로 유지

                case PKT_AUTH_REGISTER_REQ:
                    out_payload = handle_auth_signup_req(req, *conn);
                    break;

                case PKT_AUTH_VERIFY_REQ:
                    out_payload = handle_auth_verify_req(req, *conn);
                    break;

                case PKT_AUTH_LOGIN_REQ:
                    out_payload = handle_auth_login(task.sock, req, *conn);
                    break;

                case PKT_MSG_POLL_REQ:
                    out_payload = handle_msg_poll(req, *conn);
                    break;

                case PKT_MSG_SEND_REQ:
                    out_payload = handle_msg_send(req, *conn);
                    break;

                case PKT_FILE_UPLOAD_REQ:
                    out_payload = handle_file_upload_req(req, *conn);
                    break;

                case PKT_FILE_CHUNK:
                    out_payload = handle_file_chunk(req, *conn);
                    break;

                case PKT_FILE_DOWNLOAD_REQ:
                    // 다운로드는 worker가 소켓에 직접 packet_send로 청크를 쏨.
                    // ① 소켓을 blocking으로 전환 (non-blocking이면 EAGAIN 발생)
                    // ② g_streaming_socks에 등록 → epoll이 이 소켓엔 write_buf flush 안 함
                    // ③ 완료 후 복구 + write_buf에 남은 DONE 패킷 flush를 위해 EPOLLOUT 트리거
                    set_blocking(task.sock);
                    { std::lock_guard<std::mutex> lk(g_streaming_m);
                      g_streaming_socks.insert(task.sock); }
                    out_payload = handle_file_download_req(task.sock, req, *conn);
                    { std::lock_guard<std::mutex> lk(g_streaming_m);
                      g_streaming_socks.erase(task.sock); }
                    set_nonblocking(task.sock);
                    // wake_fd를 한 번 더 써서 epoll이 write_buf(DONE 패킷)를 flush하게 함
                    { uint64_t u = 1; if (g_wake_fd != -1) write(g_wake_fd, &u, sizeof(u)); }
                    break;

                case PKT_FILE_DELETE_REQ:
                    out_payload = handle_file_delete_req(req, *conn);
                    break;

                case PKT_FILE_LIST_REQ:
                    out_payload = handle_file_list_req(req, *conn);
                    break;

                case PKT_SETTINGS_GET_REQ:
                    out_payload = handle_settings_get(req, *conn);
                    break;

                case PKT_SETTINGS_SET_REQ:
                    out_payload = handle_settings_set(req, *conn);
                    break;

                case PKT_MSG_LIST_REQ:
                    out_payload = handle_msg_list(req, *conn);
                    break;

                case PKT_MSG_DELETE_REQ:
                    out_payload = handle_msg_delete(req, *conn);
                    break;

                case PKT_MSG_READ_REQ:
                    out_payload = handle_msg_read(req, *conn);
                    break;

                case PKT_MSG_SETTING_GET_REQ:
                    out_payload = handle_msg_setting_get(req, *conn);
                    break;

                case PKT_SETTINGS_VERIFY_REQ:
                    out_payload = handle_settings_verify_req(req, *conn);
                    break;

                case PKT_BLACKLIST_REQ:
                    out_payload = handle_server_blacklist_process(req, *conn);
                    break;

                case PKT_MSG_SETTING_UPDATE_REQ:
                    out_payload = handle_msg_setting_update(req, *conn);
                    break;

                case PKT_AUTH_LOGOUT_REQ:
                {
                    logout_unregister(task.sock);
                    out_payload = make_resp(
                        PKT_AUTH_LOGOUT_REQ,
                        VALUE_SUCCESS,
                        "Logged out",
                        json::object()
                    ).dump();
                    break;
                }

                case PKT_ADMIN_USER_LIST_REQ:
                    out_payload = handle_admin_user_list(req, *conn);
                    break;

                case PKT_ADMIN_USER_INFO_REQ:
                    out_payload = handle_admin_user_info(req, *conn);
                    break;

                case PKT_ADMIN_STATE_CHANGE_REQ:
                    out_payload = handle_admin_state_change(req, *conn);
                    break;

                default:
                    out_payload = make_resp(
                        type,
                        VALUE_ERR_UNKNOWN,
                        "Unknown type",
                        json::object()
                    ).dump();
                    break;

                } // switch 끝

            } // 성공 처리 끝
        }
        catch (const std::exception &e)
        {
            out_payload = make_resp(
                type,
                VALUE_ERR_UNKNOWN,
                std::string("Exception: ") + e.what(),
                json::object()
            ).dump();
        }
                catch (const std::exception &e)
        {
            out_payload = make_resp(VALUE_ERR_UNKNOWN, -1, std::string("Exception: ") + e.what(), json::object()).dump(); // 에러 응답
        } // try-catch 끝

        // 응답 페이로드 비어있으면 에러 응답으로 대체
        if (out_payload.empty())
        {
            out_payload = make_resp(type, VALUE_ERR_UNKNOWN, "empty response", json::object()).dump();
        }
        // type=17(PKT_MSG_POLL_REQ)은 폴링 전용 - 로그 생략
        if (type != PKT_MSG_POLL_REQ)
            std::cout << "[DEBUG] response type=" << type
                      << " len=" << out_payload.size()
                      << " payload=" << out_payload.substr(0, 120) << std::endl;

        {                                                       // 응답 큐 lock 블록
            std::lock_guard<std::mutex> lk(g_res_m);            // 응답 큐 lock
            g_res_q.push(ResponseTask{task.sock, out_payload}); // 응답 작업 push

        } // lock 블록 끝
        uint64_t u = 1;
        if (g_wake_fd != -1)
        {
            write(g_wake_fd, &u, sizeof(u));
        }
    } 
} 

// ============================================================================
// epoll 서버 본체
// ============================================================================
int main(int argc, char **argv)
{ // main 시작
    srand(static_cast<unsigned int>(time(NULL)));
    email_init();
    file_handler_init("./cloud_storage");
    // 초기화
    signal(SIGPIPE, SIG_IGN); // SIGPIPE 무시(끊긴 소켓 send 방지)
    int port = DEFAULT_PORT;  // 포트 기본값
    if (argc >= 2)
    {                              // 인자 있으면
        port = std::stoi(argv[1]); // 포트 파싱
    }

    std::string db_url = "jdbc:mariadb://10.10.20.108/3loud"; // DB URL 예시
    std::string db_user = "gm_3loud";                         // DB 유저 예시
    std::string db_pw = "1234";                               // DB 비번 예시

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0); // 리슨 소켓 생성
    if (listen_fd < 0)
    {                                                              // 실패 검사
        std::cerr << "socket failed: " << strerror(errno) << "\n"; // 로그
        return 1;                                                  // 종료
    }

    int opt = 1;                                                        // 소켓 옵션 값
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 재사용 옵션

    sockaddr_in addr;                                   // 주소 구조체
    memset(&addr, 0, sizeof(addr));                     // 0 초기화
    addr.sin_family = AF_INET;                          // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY);           // 모든 인터페이스
    addr.sin_port = htons(static_cast<uint16_t>(port)); // 포트 설정

    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {                                                            // 바인드
        std::cerr << "bind failed: " << strerror(errno) << "\n"; // 로그
        safe_close(listen_fd);                                   // close
        return 1;                                                // 종료
    }

    if (listen(listen_fd, LISTEN_BACKLOG) < 0)
    {                                                              // 리슨
        std::cerr << "listen failed: " << strerror(errno) << "\n"; // 로그
        safe_close(listen_fd);                                     // close
        return 1;                                                  // 종료
    }

    if (!set_nonblocking(listen_fd))
    {                                                  // 논블로킹 설정
        std::cerr << "listen_fd nonblocking failed\n"; // 로그
        safe_close(listen_fd);                         // close
        return 1;                                      // 종료
    }

    int epfd = epoll_create1(0); // epoll fd 생성
    if (epfd < 0)
    {                                                                     // 실패 검사
        std::cerr << "epoll_create1 failed: " << strerror(errno) << "\n"; // 로그
        safe_close(listen_fd);                                            // close
        return 1;                                                         // 종료
    }

    g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); // worker->epoll 깨우기용 eventfd
    if (g_wake_fd < 0)
    {                                                               // 실패 검사
        std::cerr << "eventfd failed: " << strerror(errno) << "\n"; // 로그
        safe_close(epfd);                                           // close
        safe_close(listen_fd);                                      // close
        return 1;                                                   // 종료
    }

    epoll_event ev;                                 // epoll 이벤트
    memset(&ev, 0, sizeof(ev));                     // 0 초기화
    ev.events = EPOLLIN;                            // 읽기 이벤트
    ev.data.fd = listen_fd;                         // 리슨 fd 등록
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev); // epoll에 추가

    epoll_event wkev;                                 // wake 이벤트
    memset(&wkev, 0, sizeof(wkev));                   // 0 초기화
    wkev.events = EPOLLIN;                            // 읽기 이벤트
    wkev.data.fd = g_wake_fd;                         // wake fd 등록
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_wake_fd, &wkev); // epoll에 추가

    std::unordered_map<int, Session> sessions; // 세션 맵

    static constexpr int WORKER_COUNT = 2; // 코어 1개면 2개 정도 테스트

    std::vector<std::thread> workers;

    for (int i = 0; i < WORKER_COUNT; ++i)
    {
        workers.emplace_back(worker_loop, db_url, db_user, db_pw);
    }

    std::cout << "[Server] started port=" << port << "\n"; // 서버 시작 로그

    epoll_event events[EPOLL_MAX_EVENTS]; // 이벤트 배열

    // 인증 정보 청소 주기 관리를 위한 변수 선언 (메인 루프 진입 전)
    time_t last_cleanup_time = time(NULL);
    const int CLEANUP_INTERVAL = 10; // 10초마다 청소

    while (g_running.load())
    {                                                             // 메인 루프
        int n = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 1000); // epoll 대기 (1초마다 루프 한번 돔)
        time_t now = time(NULL);                                  // 메모리 청소 로직
        if (now - last_cleanup_time >= CLEANUP_INTERVAL)
        {
            cleanup_pending_map();   // 만료된 데이터 삭제 함수 호출
            last_cleanup_time = now; // 시간 갱신
            // std::cout << "[System] Cleanup check done.\n"; // (디버깅용 로그)
        }
        if (n < 0)
        { // 실패
            if (errno == EINTR)
                continue;                                                  // 시그널이면 재시도
            std::cerr << "epoll_wait failed: " << strerror(errno) << "\n"; // 로그
            break;                                                         // 탈출
        }

        for (int i = 0; i < n; ++i)
        {                               // 이벤트 순회
            int fd = events[i].data.fd; // 이벤트 fd

            if (fd == g_wake_fd)
            {                                   // wake 이벤트면
                uint64_t u = 0;                 // 읽을 값
                read(g_wake_fd, &u, sizeof(u)); // eventfd 비우기

                std::queue<ResponseTask> local;              // 로컬 큐
                {                                            // lock 블록
                    std::lock_guard<std::mutex> lk(g_res_m); // 응답 큐 lock
                    std::swap(local, g_res_q);               // 통째로 swap해서 락 시간 최소화
                } // lock 블록 끝

                while (!local.empty())
                {                                     // 로컬 큐 처리
                    ResponseTask rt = local.front();  // front
                    local.pop();                      // pop
                    auto it = sessions.find(rt.sock); // 세션 찾기
                    if (it == sessions.end())
                        continue;            // 없으면 무시
                    Session &s = it->second; // 세션 참조

                    uint32_t len = static_cast<uint32_t>(rt.payload.size()); // payload 길이
                    if (len > static_cast<uint32_t>(MAX_PACKET_SIZE))
                    {                            // 너무 크면
                        safe_close(s.sock);      // close
                        sessions.erase(rt.sock); // 세션 제거
                        continue;                // 다음
                    }

                    uint32_t net_len = htonl(len);                                           // 네트워크 바이트 변환
                    s.write_buf.append(reinterpret_cast<char *>(&net_len), sizeof(net_len)); // 길이 추가
                    s.write_buf.append(rt.payload);                                          // payload 추가

                    // 스트리밍(다운로드) 중인 소켓은 EPOLLOUT 등록 안 함
                    // (worker가 blocking send 완료 후 set_nonblocking 복구 시 자동 처리됨)
                    {
                        std::lock_guard<std::mutex> lk(g_streaming_m);
                        if (g_streaming_socks.count(s.sock)) continue;
                    }

                    epoll_event mod;                              // 수정 이벤트
                    memset(&mod, 0, sizeof(mod));                 // 0 초기화
                    mod.events = EPOLLIN | EPOLLOUT;              // 읽기+쓰기 이벤트
                    mod.data.fd = s.sock;                         // 대상 소켓
                    epoll_ctl(epfd, EPOLL_CTL_MOD, s.sock, &mod); // epoll 수정
                } // while 끝

                continue; // 다음 이벤트
            } // wake_fd 처리 끝

            if (fd == listen_fd)
            { // 신규 접속이면
                while (true)
                {                                                                             // accept 루프(논블로킹)
                    sockaddr_in caddr;                                                        // 클라 주소
                    socklen_t clen = sizeof(caddr);                                           // 길이
                    int cfd = accept(listen_fd, reinterpret_cast<sockaddr *>(&caddr), &clen); // accept
                    if (cfd < 0)
                    { // 실패면
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;                                                 // 더 이상 없음
                        std::cerr << "accept failed: " << strerror(errno) << "\n"; // 로그
                        break;                                                     // 탈출
                    }

                    set_nonblocking(cfd); // 클라 소켓 논블로킹

                    char ipbuf[INET_ADDRSTRLEN];                               // IP 버퍼
                    inet_ntop(AF_INET, &caddr.sin_addr, ipbuf, sizeof(ipbuf)); // IP 변환

                    Session s;                           // 세션 생성
                    s.sock = cfd;                        // 소켓 저장
                    s.peer_ip = ipbuf;                   // IP 저장
                    s.peer_port = ntohs(caddr.sin_port); // 포트 저장

                    sessions.emplace(cfd, std::move(s)); // 맵에 세션 등록

                    epoll_event add;                           // epoll 등록 이벤트
                    memset(&add, 0, sizeof(add));              // 0 초기화
                    add.events = EPOLLIN;                      // 읽기 이벤트
                    add.data.fd = cfd;                         // 클라 fd
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &add); // epoll add

                    std::cout << "[Accept] fd=" << cfd << " ip=" << ipbuf << "\n"; // 로그
                } // accept while 끝
                continue; // 다음 이벤트
            } // listen_fd 처리 끝

            auto sit = sessions.find(fd); // 세션 찾기
            if (sit == sessions.end())
            {             // 없으면
                continue; // 무시
            }

            Session &s = sit->second; // 세션 참조

            if (events[i].events & EPOLLIN)
            {
                char buffer[4096]; // 임시 수신 버퍼

                // ===============================
                // 1️⃣ 수신 누적
                // ===============================
                while (true)
                {
                    int n = recv(fd, buffer, sizeof(buffer), 0);

                    if (n > 0)
                    {
                        s.read_buf.append(buffer, n); // 세션 read_buf에 누적
                    }
                    else if (n == 0)
                    {
                        logout_unregister(fd); // 로그아웃 처리
                        safe_close(fd);
                        sessions.erase(fd);
                        break;
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;

                        safe_close(fd);
                        sessions.erase(fd);
                        break;
                    }
                }

                // ===============================
                // 2️⃣ 프레이밍 (length-prefix 복원)
                // ===============================
                while (true)
                {
                    if (s.read_buf.size() < 4)
                        break;

                    uint32_t net_len;
                    memcpy(&net_len, s.read_buf.data(), 4);

                    uint32_t len = ntohl(net_len);

                    if (len > MAX_PACKET_SIZE)
                    {
                        safe_close(fd);
                        sessions.erase(fd);
                        break;
                    }

                    if (s.read_buf.size() < 4 + len)
                        break;

                    std::string payload = s.read_buf.substr(4, len);

                    s.read_buf.erase(0, 4 + len);

                    {
                        std::lock_guard<std::mutex> lk(g_req_m);
                        g_req_q.push(Task{fd, payload});
                    }

                    g_req_cv.notify_one();
                }
            } // EPOLLIN 처리 끝

            if (events[i].events & EPOLLOUT)
            { // 쓰기 이벤트면
                // 다운로드 스트리밍 중인 소켓은 worker가 직접 send() 중이므로
                // epoll이 동시에 write_buf를 flush하면 데이터가 섞임 → 건너뜀
                {
                    std::lock_guard<std::mutex> lk(g_streaming_m);
                    if (g_streaming_socks.count(fd)) continue;
                }
                if (!s.write_buf.empty())
                {                                                                            // 보낼게 있으면
                    int n3 = send(fd, s.write_buf.data(), s.write_buf.size(), MSG_DONTWAIT); // send
                    if (n3 > 0)
                    {                                                  // 보냈으면
                        s.write_buf.erase(0, static_cast<size_t>(n3)); // 보낸만큼 제거
                    }
                    else if (n3 == 0)
                    {
                        logout_unregister(fd); // 로그아웃 처리
                        safe_close(fd);        // close
                        sessions.erase(fd);    // 제거
                        continue;              // 다음
                    }
                    else
                    { // n3 < 0
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                        {                       // 진짜 에러면
                            safe_close(fd);     // close
                            sessions.erase(fd); // 제거
                            continue;           // 다음
                        }
                    } // else 끝
                }

                if (s.write_buf.empty())
                {                                             // 다 보냈으면
                    epoll_event mod;                          // 이벤트 수정
                    memset(&mod, 0, sizeof(mod));             // 0 초기화
                    mod.events = EPOLLIN;                     // 다시 읽기만
                    mod.data.fd = fd;                         // 대상 fd
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &mod); // epoll mod
                }
            } // EPOLLOUT 처리 끝
        } // for 끝
    } // while 끝

    g_running = false;     // 종료 플래그 내리기
    g_req_cv.notify_all(); // worker 깨우기

    for (auto &th : workers)
    { // 생성한 워커 스레드들 순회
        if (th.joinable())
        {              // join 가능한지 확인
            th.join(); // 스레드 join
        }
    }
    for (auto &kv : sessions)
    {                               // 남은 세션 정리
        safe_close(kv.second.sock); // close
    } // for 끝

    safe_close(g_wake_fd); // wake close
    safe_close(epfd);      // epoll close
    safe_close(listen_fd); // listen close

    std::cout << "[Server] stopped\n"; // 종료 로그
    return 0;                          // main 종료
} // main 끝