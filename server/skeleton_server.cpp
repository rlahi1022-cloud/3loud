// ============================================================================ // 구분 주석
// 파일명: server.cpp                                                          // 파일명 설명
// 목적: epoll 기반 메인루프 + worker thread + DB(Worker 전용) + length-prefix // 목적 설명
// 전제: common/packet.c, common/packet.h 를 공용 모듈로 사용                  // 전제 설명
// 플랫폼: Linux                                                                // 플랫폼 설명
// ============================================================================ // 구분 주석

#include <iostream>                         // 표준 입출력 사용
#include <string>                           // std::string 사용
#include <vector>                           // std::vector 사용
#include <unordered_map>                    // 세션 맵 사용
#include <queue>                            // 큐 사용
#include <mutex>                            // mutex 사용
#include <condition_variable>               // condition_variable 사용
#include <thread>                           // thread 사용
#include <atomic>                           // atomic 사용
#include <cstring>                          // memset, memcpy 사용
#include <cerrno>                           // errno 사용
#include <csignal>                          // signal 사용
#include <unistd.h>                         // close, read, write 사용
#include <fcntl.h>                          // fcntl 사용
#include <sys/socket.h>                     // socket 관련
#include <netinet/in.h>                     // sockaddr_in
#include <arpa/inet.h>                      // inet_ntop, inet_pton
#include <sys/epoll.h>                      // epoll
#include <sys/eventfd.h>                    // eventfd
#include <nlohmann/json.hpp>                // JSON 라이브러리 사용
#include <mariadb/conncpp.hpp>              // MariaDB C++ Connector 사용
#include "protocol/protocol.h"

extern "C" {                                // C 모듈을 C 링크로 사용
#include "packet.h"                         // length-prefix send/recv 공용 모듈
}                                           // extern "C" 끝

#include "../server_handle/file_handler.hpp" 

using json = nlohmann::json;                // json 타입 별칭

static constexpr int EPOLL_MAX_EVENTS = 128; // epoll 이벤트 배열 크기
static constexpr int MAX_PACKET_SIZE = 10 * 1024 * 1024; // 최대 패킷 크기 제한(10MB)
static constexpr int DEFAULT_PORT = 5010;   // 기본 포트
static constexpr int LISTEN_BACKLOG = 64;   // listen backlog

// ============================================================================ // 구분 주석
// 세션 구조체: epoll 스레드에서만 접근/수정하는 것을 기본 원칙으로 둠
// write_buf는 epoll 스레드가 flush 하며, worker는 응답 큐에만 넣음
// ============================================================================ // 구분 주석

struct Session {                            // 세션 구조체 시작
    int sock = -1;                          // 클라이언트 소켓 fd
    std::string peer_ip;                    // 클라이언트 IP 문자열
    uint16_t peer_port = 0;                 // 클라이언트 포트
    std::string write_buf;                  // 전송 대기 버퍼
    std::string read_buf;
};                                          // 세션 구조체 끝

// ============================================================================ // 구분 주석
// Task / Response: epoll -> worker / worker -> epoll 교환용
// ============================================================================ // 구분 주석

struct Task {                               // 작업 요청 구조체 시작
    int sock = -1;                          // 요청이 온 소켓
    std::string payload;                    // JSON 문자열 payload
};                                          // 작업 요청 구조체 끝

struct ResponseTask {                       // 응답 작업 구조체 시작
    int sock = -1;                          // 응답 보낼 소켓
    std::string payload;                    // JSON 문자열 payload
};                                          // 응답 작업 구조체 끝

// ============================================================================ // 구분 주석
// 전역(공유) 큐: worker 스레드와 epoll 스레드가 공유하므로 mutex로 보호
// ============================================================================ // 구분 주석

static std::queue<Task> g_req_q;            // 요청 큐
static std::queue<ResponseTask> g_res_q;    // 응답 큐
static std::mutex g_req_m;                  // 요청 큐 mutex
static std::mutex g_res_m;                  // 응답 큐 mutex
static std::condition_variable g_req_cv;    // worker를 깨우는 CV
static std::atomic<bool> g_running(true);   // 서버 실행 플래그(원자)
static int g_wake_fd = -1;                  // worker→epoll 신호용 eventfd (전역)

// ============================================================================ // 구분 주석
// 유틸: non-blocking 설정
// ============================================================================ // 구분 주석

static bool set_nonblocking(int fd) {       // 논블로킹 설정 함수 시작
    int flags = fcntl(fd, F_GETFL, 0);      // 현재 플래그 읽기
    if (flags < 0) return false;            // 실패 시 false
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false; // O_NONBLOCK 추가
    return true;                            // 성공
}                                           // 함수 끝

// ============================================================================ // 구분 주석
// 유틸: 안전한 close + 에러 무시
// ============================================================================ // 구분 주석

static void safe_close(int fd) {            // 안전 close 함수 시작
    if (fd >= 0) {                          // 유효 fd인지 확인
        ::close(fd);                        // close 호출
    }                                       // if 끝
}                                           // 함수 끝

// ============================================================================ // 구분 주석
// JSON 응답 생성 helper: 팀원이 핸들에서 이걸로 통일하면 좋음
// ============================================================================ // 구분 주석

static std::string make_resp(int type,      // 응답 타입
                             int code,      // 내부 코드(성공=0 등)
                             const std::string& msg, // 메시지
                             const json& payload) {  // payload
    json res;                               // 응답 JSON 생성
    res["type"] = type;                     // type 세팅
    res["code"] = code;                     // code 세팅
    res["msg"] = msg;                       // msg 세팅
    res["payload"] = payload;               // payload 세팅
    return res.dump();                      // JSON 문자열로 반환
}                                           // 함수 끝

// // ============================================================================ // 구분 주석
// // 핸들 함수 자리: 팀원들이 이 함수들만 작성하면 됨
// // DB 커넥션은 worker thread 안에서만 사용(요구사항 YES)
// // ============================================================================ // 구분 주석

// static std::string handle_auth_signup(const json& req, sql::Connection& db) { // 회원가입 핸들
//     json payload = req.value("payload", json::object());                      // payload 방어 파싱
//     std::string email = payload.value("email", "");                           // email 디폴트 빈문자
//     std::string pw_hash = payload.value("pw_hash", "");                       // pw_hash 디폴트 빈문자
//     std::string name = payload.value("name", "");                             // name 디폴트 빈문자
//     if (email.empty() || pw_hash.empty() || name.empty()) {                   // 필수값 누락 검사
//         return make_resp(PKT_RESP_ERR, -1, "필수 필드 누락", json::object());  // 에러 응답
//     }                                                                         // if 끝
//     // 여기서 DB INSERT / 이메일 인증 흐름 등 실제 구현 들어가면 됨              // 구현 안내
//     return make_resp(PKT_RESP_OK, 0, "signup placeholder", json::object());   // 임시 성공 응답
// }                                                                              // 함수 끝

// static std::string handle_auth_login(const json& req, sql::Connection& db) {  // 로그인 핸들
//     json payload = req.value("payload", json::object());                      // payload 방어 파싱
//     std::string email = payload.value("email", "");                           // email
//     std::string pw_hash = payload.value("pw_hash", "");                       // pw_hash
//     if (email.empty() || pw_hash.empty()) {                                   // 필수값 확인
//         return make_resp(PKT_RESP_ERR, -1, "필수 필드 누락", json::object());  // 에러 응답
//     }                                                                         // if 끝
//     // DB SELECT 비교 / 세션 토큰 발급 등 구현                                   // 구현 안내
//     json out;                                                                  // payload 만들기
//     out["user"] = { {"email", email} };                                        // 유저 정보 예시
//     return make_resp(PKT_RESP_OK, 0, "login placeholder", out);                // 임시 성공 응답
// }                                                                              // 함수 끝

// static std::string handle_msg_send(const json& req, sql::Connection& db) {    // 메시지 전송 핸들
//     json payload = req.value("payload", json::object());                      // payload
//     std::string to = payload.value("to", "");                                 // 받는 사람
//     std::string content = payload.value("content", "");                       // 내용
//     if (to.empty() || content.empty()) {                                      // 필수값 확인
//         return make_resp(PKT_RESP_ERR, -1, "필수 필드 누락", json::object());  // 에러
//     }                                                                         // if 끝
//     // DB INSERT messages 구현                                                  // 구현 안내
//     return make_resp(PKT_RESP_OK, 0, "msg_send placeholder", json::object()); // 임시 성공
// }                                                                              // 함수 끝

// ============================================================================ // 구분 주석
// Worker Thread: 요청 처리 담당 (DB 연결은 여기서 생성해서 전용으로 사용)
// ============================================================================ // 구분 주석

// ============================================================================
// Worker Thread (안정화 + 구조 정리 완료 버전)
// ============================================================================
static void worker_loop(std::string db_url,
                        std::string db_user,
                        std::string db_pw)
{
    sql::Driver* driver = nullptr;
    std::unique_ptr<sql::Connection> conn;

    try {
        driver = sql::mariadb::get_driver_instance();

        sql::Properties props({
            {"user", db_user},
            {"password", db_pw}
        });

        conn.reset(driver->connect(db_url, props));

        {
            std::unique_ptr<sql::Statement> st(conn->createStatement());
            st->execute("SET NAMES 'utf8mb4'");
        }

        std::cout << "[Worker] DB connected\n";
    }
    catch (const sql::SQLException& e) {
        std::cerr << "[Worker] DB connect failed: " << e.what() << "\n";
        g_running = false;
        return;
    }

    while (g_running.load())
    {
        Task task;

        // ===============================
        // 요청 큐 대기
        // ===============================
        {
            std::unique_lock<std::mutex> lk(g_req_m);

            g_req_cv.wait(lk, [] {
                return !g_req_q.empty() || !g_running.load();
            });

            if (!g_running.load())
                break;

            task = g_req_q.front();
            g_req_q.pop();
        }

        std::string out_payload;

        // ===============================
        // JSON 파싱 + 핸들 분기
        // ===============================
        try
        {
            json req = json::parse(task.payload);
            int type = req.value("type", 0);

            switch (type)
            {
                case PKT_AUTH_REGISTER_REQ:
                    out_payload = handle_signup(req, *conn);
                    break;

                case PKT_AUTH_LOGIN_REQ:
                    out_payload = handle_login(req, *conn);
                    break;

                case PKT_AUTH_LOGOUT_REQ:
                    out_payload = handle_logout(req, *conn);
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
                    out_payload = handle_file_download_req(task.sock, req, *conn);
                    break;

                case PKT_FILE_DELETE_REQ:
                    out_payload = handle_file_delete_req(req, *conn);
                    break;

                case PKT_FILE_LIST_REQ:
                    out_payload = handle_file_list_req(req, *conn);
                    break;

                default:
                    out_payload = make_resp(type, -1, "Unknown type");
                    break;
            }
        }
        catch (const std::exception& e)
        {
            out_payload = make_resp(
                0,
                -1,
                std::string("json parse error: ") + e.what()
            );
        }

        // ===============================
        // 응답 큐 push
        // ===============================
        if (!out_payload.empty())
        {
            {
                std::lock_guard<std::mutex> lk(g_res_m);
                g_res_q.push(ResponseTask{ task.sock, out_payload });
            }

            // ===============================
            // epoll 깨우기 (eventfd)
            // ===============================
            uint64_t val = 1;
            int wr = write(g_wake_fd, &val, sizeof(val));

            if (wr < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    std::cerr << "eventfd write failed: "
                              << strerror(errno) << "\n";
                    g_running = false;
                }
            }
        }
    }
}

// ============================================================================ // 구분 주석
// epoll 서버 본체
// ============================================================================ // 구분 주석

int main(int argc, char** argv) {                                                     // main 시작
    file_handler_init(std::string(getenv("HOME")) + "/3loud_files");
    signal(SIGPIPE, SIG_IGN);                                                         // SIGPIPE 무시(끊긴 소켓 send 방지)
    int port = DEFAULT_PORT;                                                          // 포트 기본값
    if (argc >= 2) {                                                                  // 인자 있으면
        port = std::stoi(argv[1]);                                                    // 포트 파싱
    }                                                                                 // if 끝

    std::string db_url = "jdbc:mariadb://10.10.20.108/3loud";                       // DB URL 예시
    std::string db_user = "gm_3loud";                                                 // DB 유저 예시
    std::string db_pw = "1234";                                                       // DB 비번 예시

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);                                  // 리슨 소켓 생성
    if (listen_fd < 0) {                                                              // 실패 검사
        std::cerr << "socket failed: " << strerror(errno) << "\n";                    // 로그
        return 1;                                                                     // 종료
    }                                                                                 // if 끝

    int opt = 1;                                                                      // 소켓 옵션 값
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));               // 재사용 옵션

    sockaddr_in addr;                                                                 // 주소 구조체
    memset(&addr, 0, sizeof(addr));                                                   // 0 초기화
    addr.sin_family = AF_INET;                                                        // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY);                                         // 모든 인터페이스
    addr.sin_port = htons(static_cast<uint16_t>(port));                               // 포트 설정

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {      // 바인드
        std::cerr << "bind failed: " << strerror(errno) << "\n";                      // 로그
        safe_close(listen_fd);                                                        // close
        return 1;                                                                     // 종료
    }                                                                                 // if 끝

    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {                                      // 리슨
        std::cerr << "listen failed: " << strerror(errno) << "\n";                    // 로그
        safe_close(listen_fd);                                                        // close
        return 1;                                                                     // 종료
    }                                                                                 // if 끝

    if (!set_nonblocking(listen_fd)) {                                                // 논블로킹 설정
        std::cerr << "listen_fd nonblocking failed\n";                                // 로그
        safe_close(listen_fd);                                                        // close
        return 1;                                                                     // 종료
    }                                                                                 // if 끝

    int epfd = epoll_create1(0);                                                      // epoll fd 생성
    if (epfd < 0) {                                                                   // 실패 검사
        std::cerr << "epoll_create1 failed: " << strerror(errno) << "\n";             // 로그
        safe_close(listen_fd);                                                        // close
        return 1;                                                                     // 종료
    }                                                                                 // if 끝

    g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);                               // worker->epoll 깨우기용 eventfd
    if (g_wake_fd < 0) {                                                                // 실패 검사
        std::cerr << "eventfd failed: " << strerror(errno) << "\n";                   // 로그
        safe_close(epfd);                                                             // close
        safe_close(listen_fd);                                                        // close
        return 1;                                                                     // 종료
    }                                                                                 // if 끝

    epoll_event ev;                                                                   // epoll 이벤트
    memset(&ev, 0, sizeof(ev));                                                       // 0 초기화
    ev.events = EPOLLIN;                                                              // 읽기 이벤트
    ev.data.fd = listen_fd;                                                           // 리슨 fd 등록
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);                                   // epoll에 추가

    epoll_event wkev;                                                                 // wake 이벤트
    memset(&wkev, 0, sizeof(wkev));                                                   // 0 초기화
    wkev.events = EPOLLIN;                                                            // 읽기 이벤트
    wkev.data.fd = g_wake_fd;                                                           // wake fd 등록
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_wake_fd, &wkev);                                   // epoll에 추가

    std::unordered_map<int, Session> sessions;                                        // 세션 맵

    static constexpr int WORKER_COUNT = 2; // 코어 1개면 2개 정도 테스트

    std::vector<std::thread> workers;

    for (int i = 0; i < WORKER_COUNT; ++i)
    {
        workers.emplace_back(worker_loop, db_url, db_user, db_pw);
    }

    std::cout << "[Server] started port=" << port << "\n";                            // 서버 시작 로그

    epoll_event events[EPOLL_MAX_EVENTS];                                             // 이벤트 배열

    while (g_running.load()) {                                                        // 메인 루프
        int n = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, -1);                       // epoll 대기
        if (n < 0) {                                                                  // 실패
            if (errno == EINTR) continue;                                             // 시그널이면 재시도
            std::cerr << "epoll_wait failed: " << strerror(errno) << "\n";            // 로그
            break;                                                                    // 탈출
        }                                                                             // if 끝

        for (int i = 0; i < n; ++i) {                                                 // 이벤트 순회
            int fd = events[i].data.fd;                                               // 이벤트 fd

            if (fd == g_wake_fd) {                                                      // wake 이벤트면
                uint64_t u = 0;                                                       // 읽을 값
                read(g_wake_fd, &u, sizeof(u));                                         // eventfd 비우기

                std::queue<ResponseTask> local;                                       // 로컬 큐
                {                                                                     // lock 블록
                    std::lock_guard<std::mutex> lk(g_res_m);                          // 응답 큐 lock
                    std::swap(local, g_res_q);                                        // 통째로 swap해서 락 시간 최소화
                }                                                                     // lock 블록 끝

                while (!local.empty()) {                                              // 로컬 큐 처리
                    ResponseTask rt = local.front();                                  // front
                    local.pop();                                                      // pop
                    auto it = sessions.find(rt.sock);                                 // 세션 찾기
                    if (it == sessions.end()) continue;                               // 없으면 무시
                    Session& s = it->second;                                          // 세션 참조

                    uint32_t len = static_cast<uint32_t>(rt.payload.size());          // payload 길이
                    if (len > static_cast<uint32_t>(MAX_PACKET_SIZE)) {              // 너무 크면
                        safe_close(s.sock);                                           // close
                        sessions.erase(rt.sock);                                      // 세션 제거
                        continue;                                                     // 다음
                    }                                                                 // if 끝

                    uint32_t net_len = htonl(len);                                    // 네트워크 바이트 변환
                    s.write_buf.append(reinterpret_cast<char*>(&net_len), sizeof(net_len)); // 길이 추가
                    s.write_buf.append(rt.payload);                                   // payload 추가

                    epoll_event mod;                                                  // 수정 이벤트
                    memset(&mod, 0, sizeof(mod));                                     // 0 초기화
                    mod.events = EPOLLIN | EPOLLOUT;                                  // 읽기+쓰기 이벤트
                    mod.data.fd = s.sock;                                             // 대상 소켓
                    epoll_ctl(epfd, EPOLL_CTL_MOD, s.sock, &mod);                     // epoll 수정
                }                                                                     // while 끝

                continue;                                                             // 다음 이벤트
            }                                                                         // wake_fd 처리 끝

            if (fd == listen_fd) {                                                    // 신규 접속이면
                while (true) {                                                        // accept 루프(논블로킹)
                    sockaddr_in caddr;                                                // 클라 주소
                    socklen_t clen = sizeof(caddr);                                   // 길이
                    int cfd = accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen); // accept
                    if (cfd < 0) {                                                    // 실패면
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;           // 더 이상 없음
                        std::cerr << "accept failed: " << strerror(errno) << "\n";    // 로그
                        break;                                                        // 탈출
                    }                                                                 // if 끝

                    set_nonblocking(cfd);                                             // 클라 소켓 논블로킹

                    char ipbuf[INET_ADDRSTRLEN];                                      // IP 버퍼
                    inet_ntop(AF_INET, &caddr.sin_addr, ipbuf, sizeof(ipbuf));        // IP 변환

                    Session s;                                                        // 세션 생성
                    s.sock = cfd;                                                     // 소켓 저장
                    s.peer_ip = ipbuf;                                                // IP 저장
                    s.peer_port = ntohs(caddr.sin_port);                              // 포트 저장

                    sessions.emplace(cfd, std::move(s));                              // 맵에 세션 등록

                    epoll_event add;                                                  // epoll 등록 이벤트
                    memset(&add, 0, sizeof(add));                                     // 0 초기화
                    add.events = EPOLLIN;                                             // 읽기 이벤트
                    add.data.fd = cfd;                                                // 클라 fd
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &add);                        // epoll add

                    std::cout << "[Accept] fd=" << cfd << " ip=" << ipbuf << "\n";    // 로그
                }                                                                     // accept while 끝
                continue;                                                             // 다음 이벤트
            }                                                                         // listen_fd 처리 끝

            auto sit = sessions.find(fd);                                             // 세션 찾기
            if (sit == sessions.end()) {                                              // 없으면
                continue;                                                             // 무시
            }                                                                         // if 끝

            Session& s = sit->second;                                                 // 세션 참조

            if (events[i].events & EPOLLIN)
            {
                char buffer[4096];  // 임시 수신 버퍼

                // ===============================
                // 1️⃣ 수신 누적
                // ===============================
                while (true)
                {
                    int n = recv(fd, buffer, sizeof(buffer), 0);

                    if (n > 0)
                    {
                        s.read_buf.append(buffer, n);  // 세션 read_buf에 누적
                    }
                    else if (n == 0)
                    {
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
            }                                                          // EPOLLIN 처리 끝

            if (events[i].events & EPOLLOUT) {                                        // 쓰기 이벤트면
                if (!s.write_buf.empty()) {                                           // 보낼게 있으면
                    int n3 = send(fd, s.write_buf.data(), s.write_buf.size(), MSG_DONTWAIT); // send
                    if (n3 > 0) {                                                     // 보냈으면
                        s.write_buf.erase(0, static_cast<size_t>(n3));                // 보낸만큼 제거
                    } else if (n3 == 0) {                                             // 0이면 종료 취급
                        safe_close(fd);                                               // close
                        sessions.erase(fd);                                           // 제거
                        continue;                                                     // 다음
                    } else {                                                          // n3 < 0
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {                // 진짜 에러면
                            safe_close(fd);                                           // close
                            sessions.erase(fd);                                       // 제거
                            continue;                                                 // 다음
                        }                                                             // if 끝
                    }                                                                 // else 끝
                }                                                                     // if 끝

                if (s.write_buf.empty()) {                                            // 다 보냈으면
                    epoll_event mod;                                                  // 이벤트 수정
                    memset(&mod, 0, sizeof(mod));                                     // 0 초기화
                    mod.events = EPOLLIN;                                             // 다시 읽기만
                    mod.data.fd = fd;                                                 // 대상 fd
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &mod);                         // epoll mod
                }                                                                     // if 끝
            }                                                                         // EPOLLOUT 처리 끝
        }                                                                             // for 끝
    }                                                                                 // while 끝

    g_running = false;                                                                // 종료 플래그 내리기
    g_req_cv.notify_all();                                                            // worker 깨우기

    if (worker.joinable()) worker.join();                                             // worker join

    for (auto& kv : sessions) {                                                       // 남은 세션 정리
        safe_close(kv.second.sock);                                                   // close
    }                                                                                 // for 끝

    safe_close(g_wake_fd);                                                              // wake close
    safe_close(epfd);                                                                 // epoll close
    safe_close(listen_fd);                                                            // listen close

    std::cout << "[Server] stopped\n";                                                // 종료 로그
    return 0;                                                                         // main 종료
}                                                                                     // main 끝