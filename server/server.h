#include <iostream>
#include <cstring>
#include <vector>
#include <map>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <csignal>
#include <regex>
#include <cstdlib>
#include <ctime>

#ifndef SERVER_H
#define SERVER_H

// =========================================================
// [전역] 인증 대기 메모리 저장소 (DB 대신 임시 저장)
// =========================================================

struct PendingInfo //  대기중인 가입 정보 구조체
{
    std::string pw;       // 비밀번호 (아직 해싱 전)
    std::string nickname; // 닉네임
    std::string code;     // 인증번호
    time_t created_at;    // 생성 시간 (만료 체크용, 선택사항)
    time_t timestamp;
};

static std::map<std::string, PendingInfo> g_pending_map; // Key: Email
static std::mutex g_pending_m;                           // Mutex

// [유틸] 이메일 유효성 검사
static bool isValidEmail(const std::string &email)
{
    const std::regex pattern("(\\w+)(\\.|_)?(\\w*)@(\\w+)(\\.(\\w+))+");
    return std::regex_match(email, pattern);
}

// [유틸] 인증번호 생성
static std::string generate_verification_code()
{
    srand(time(NULL));
    int code = rand() % 900000 + 100000; // 6자리 난수
    return std::to_string(code);
}

// [유틸] 파이썬 메일 전송 (Blocking 방지를 위해 개선 필요하지만 일단 유지)
static bool send_email_via_python(const std::string &email, const std::string &code)
{
    // 보안상 실제 서비스에선 system() 사용을 권장하지 않음
    std::string cmd = "python3 mail.py " + email + " " + code + " > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    return (ret == 0);
}

#endif // 헤더가드