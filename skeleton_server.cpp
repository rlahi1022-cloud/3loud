#include <arpa/inet.h>          // sockaddr_in, htons 등 네트워크 구조체 사용
#include <errno.h>              // errno 사용
#include <fcntl.h>              // fcntl 함수 사용 (논블로킹 설정)
#include <netinet/in.h>         // 네트워크 주소 구조체
#include <sys/epoll.h>          // epoll 관련 함수
#include <sys/socket.h>         // socket, bind, listen, accept
#include <unistd.h>             // close 함수

#include <atomic>               // atomic 변수 사용
#include <condition_variable>   // condition_variable 사용
#include <cstring>              // strerror 사용
#include <deque>                // send 큐 자료구조
#include <functional>           // std::function 사용
#include <iostream>             // cout, cerr 사용
#include <mutex>                // mutex 사용
#include <queue>                // 작업 큐 사용
#include <string>               // string 사용
#include <thread>               // thread 사용
#include <unordered_map>        // fd → connection 매핑
#include <vector>               // epoll 이벤트 배열

#include "nlohmann/json.hpp"    // JSON 파싱 라이브러리
using json = nlohmann::json;    // json 별칭 정의

static bool set_nonblocking(int fd) {                 // 소켓을 논블로킹으로 변경하는 함수
    int flags = fcntl(fd, F_GETFL, 0);                // 현재 플래그 가져오기
    if (flags < 0) return false;                      // 실패 시 false 반환
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)   // O_NONBLOCK 플래그 추가
        return false;                                 // 실패 시 false
    return true;                                      // 성공
}

class ThreadPool {                                    // 스레드 풀 클래스 정의
public:
    explicit ThreadPool(size_t n) : stop_(false) {    // 생성자 (스레드 개수 n)
        for (size_t i = 0; i < n; ++i) {              // n개 스레드 생성
            workers_.emplace_back([this]() {          // 각 스레드는 무한 루프 실행
                for (;;) {                            // 작업 대기 루프
                    std::function<void()> task;       // 실행할 작업 객체

                    {                                 // 임계 구역 시작
                        std::unique_lock<std::mutex> lk(mu_);  // mutex 잠금
                        cv_.wait(lk, [this]() {       // 작업이 생길 때까지 대기
                            return stop_ || !q_.empty();
                        });
                        if (stop_ && q_.empty())      // 종료 조건 확인
                            return;
                        task = std::move(q_.front()); // 큐에서 작업 꺼내기
                        q_.pop();                     // 큐에서 제거
                    }                                 // 임계 구역 종료

                    task();                           // 실제 작업 실행
                }
            });
        }
    }

    ~ThreadPool() {                                   // 소멸자
        {                                             
            std::lock_guard<std::mutex> lk(mu_);      // mutex 잠금
            stop_ = true;                              // 종료 플래그 설정
        }
        cv_.notify_all();                              // 모든 스레드 깨우기
        for (auto& t : workers_)                       // 모든 스레드 순회
            if (t.joinable()) t.join();                // join 처리
    }

    void enqueue(std::function<void()> fn) {          // 작업 추가 함수
        {                                             
            std::lock_guard<std::mutex> lk(mu_);      // mutex 잠금
            q_.push(std::move(fn));                   // 큐에 작업 추가
        }
        cv_.notify_one();                             // 스레드 하나 깨움
    }

private:
    std::vector<std::thread> workers_;                // 워커 스레드 목록
    std::mutex mu_;                                   // 큐 보호 mutex
    std::condition_variable cv_;                      // 대기용 condition variable
    std::queue<std::function<void()>> q_;             // 작업 큐
    bool stop_;                                       // 종료 플래그
};

struct Connection {                                   // 클라이언트 연결 상태 구조체
    int fd = -1;                                       // 소켓 fd
    std::string recv_buf;                               // 수신 버퍼
    std::deque<std::string> send_q;                     // 송신 대기 큐
    std::mutex send_mu;                                 // 송신 보호 mutex
};

static bool extract_line(std::string& buf, std::string& out) { // 한 줄 JSON 추출 함수
    size_t pos = buf.find('\n');                         // 개행 위치 찾기
    if (pos == std::string::npos) return false;         // 없으면 false
    out = buf.substr(0, pos);                            // 한 줄 추출
    buf.erase(0, pos + 1);                               // 추출 부분 삭제
    return true;                                         // 성공
}

class Server {                                           // 서버 클래스
public:
    Server(uint16_t port, size_t worker_n)               // 생성자
        : port_(port), pool_(worker_n) {}                // 포트 및 스레드풀 초기화

    bool start() {                                       // 서버 시작 함수
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);    // TCP 소켓 생성
        if (listen_fd_ < 0) return false;                 // 실패 시 종료

        int opt = 1;                                     
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,  // 재사용 옵션 설정
                   &opt, sizeof(opt));

        sockaddr_in addr{};                              // 주소 구조체 초기화
        addr.sin_family = AF_INET;                       // IPv4
        addr.sin_addr.s_addr = INADDR_ANY;               // 모든 IP 허용
        addr.sin_port = htons(port_);                    // 포트 설정

        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            return false;                                // bind 실패

        if (listen(listen_fd_, 128) < 0)
            return false;                                // listen 실패

        set_nonblocking(listen_fd_);                     // 논블로킹 설정

        epfd_ = epoll_create1(0);                        // epoll 인스턴스 생성
        if (epfd_ < 0) return false;

        epoll_event ev{};                                
        ev.events = EPOLLIN;                             // 읽기 이벤트 등록
        ev.data.fd = listen_fd_;                         
        epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev);// epoll에 추가

        running_ = true;                                 
        loop();                                          
        return true;                                     
    }

private:
    void loop() {                                        // epoll 이벤트 루프
        std::vector<epoll_event> events(256);            // 이벤트 배열

        while (running_) {                               
            int n = epoll_wait(epfd_, events.data(), 256, 1000); // 이벤트 대기

            for (int i = 0; i < n; ++i) {                
                int fd = events[i].data.fd;              
                uint32_t ev = events[i].events;          

                if (fd == listen_fd_) {                  
                    handle_accept();                     
                    continue;                            
                }

                if (ev & EPOLLIN) handle_read(fd);       // 읽기 이벤트
                if (ev & EPOLLOUT) handle_write(fd);     // 쓰기 이벤트
                if (ev & (EPOLLERR | EPOLLHUP))          
                    close(fd);                           // 오류 시 종료
            }
        }
    }

    void handle_accept() {                               // 클라이언트 접속 처리
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);

        int cfd = accept(listen_fd_, (sockaddr*)&caddr, &clen); // 클라이언트 수락
        if (cfd < 0) return;

        set_nonblocking(cfd);                            // 논블로킹 설정

        auto conn = std::make_shared<Connection>();      
        conn->fd = cfd;                                  

        {                                               
            std::lock_guard<std::mutex> lk(conns_mu_);  
            conns_[cfd] = conn;                          
        }

        epoll_event ev{};                                
        ev.events = EPOLLIN;                             
        ev.data.fd = cfd;                                
        epoll_ctl(epfd_, EPOLL_CTL_ADD, cfd, &ev);       
    }

    void handle_read(int fd) {                           // 데이터 수신 처리
        auto conn = conns_[fd];                          
        char buf[4096];                                  

        ssize_t r = recv(fd, buf, sizeof(buf), 0);       // 데이터 읽기
        if (r <= 0) return;

        conn->recv_buf.append(buf, r);                   // 버퍼에 추가

        std::string line;                                
        if (extract_line(conn->recv_buf, line)) {        
            pool_.enqueue([this, fd, line]() {           // worker로 전달
                process_packet(fd, line);                
            });
        }
    }

    void handle_write(int fd) {                          // 데이터 송신 처리
        auto conn = conns_[fd];                          
        std::lock_guard<std::mutex> lk(conn->send_mu);   

        if (!conn->send_q.empty()) {                     
            auto& msg = conn->send_q.front();            
            send(fd, msg.data(), msg.size(), 0);         
            conn->send_q.pop_front();                    
        }
    }

    void process_packet(int fd, const std::string& line) { // 패킷 처리 함수
        json req = json::parse(line);                     
        int type = req["type"];                           
        int req_id = req["req_id"];                       

        json res;                                         
        res["type"] = type | 0x8000;                     
        res["req_id"] = req_id;                          
        res["result"] = 0;                               
        res["payload"] = { {"msg", "ok"} };              

        auto conn = conns_[fd];                           
        std::lock_guard<std::mutex> lk(conn->send_mu);   
        conn->send_q.push_back(res.dump() + "\n");       
    }

    uint16_t port_;                                      
    int listen_fd_;                                      
    int epfd_;                                           
    std::atomic<bool> running_;                          

    ThreadPool pool_;                                    

    std::mutex conns_mu_;                                
    std::unordered_map<int, std::shared_ptr<Connection>> conns_;
};

int main() {                                            
    Server s(9000, 4);                                   // 포트 9000, 워커 4개
    s.start();                                           
    return 0;                                            
}