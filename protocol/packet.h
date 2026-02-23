#include "packet.h"             // 자신의 헤더 포함
#include <arpa/inet.h>          // htonl, ntohl 사용을 위한 헤더
#include <unistd.h>             // send, recv 함수 사용
#include <stdlib.h>             // malloc, free 사용
#include <string.h>             // memset 사용

// ===============================
// 내부 함수: 정확히 n바이트 전송
// ===============================
static int send_all(int sock,           // 소켓
                    const char* buf,    // 보낼 데이터
                    uint32_t len)       // 보낼 길이
{
    uint32_t total_sent = 0;            // 지금까지 보낸 총 바이트 수

    while (total_sent < len)            // 아직 다 못 보냈다면 반복
    {
        int n = send(sock,              // 소켓에
                     buf + total_sent,  // 아직 안 보낸 부분부터
                     len - total_sent,  // 남은 길이만큼
                     0);                // flags = 0

        if (n <= 0)                     // send 실패 또는 연결 종료
            return -1;                  // 실패 반환

        total_sent += n;                // 보낸 만큼 누적
    }

    return 0;                           // 성공
}

// ===============================
// 내부 함수: 정확히 n바이트 수신
// ===============================
static int recv_all(int sock,           // 소켓
                    char* buf,          // 저장할 버퍼
                    uint32_t len)       // 받아야 할 길이
{
    uint32_t total_recv = 0;            // 지금까지 받은 총 바이트 수

    while (total_recv < len)            // 아직 다 못 받았다면 반복
    {
        int n = recv(sock,              // 소켓에서
                     buf + total_recv,  // 아직 안 채운 위치부터
                     len - total_recv,  // 남은 길이만큼
                     0);                // flags = 0

        if (n <= 0)                     // recv 실패 또는 연결 종료
            return -1;                  // 실패 반환

        total_recv += n;                // 받은 만큼 누적
    }

    return 0;                           // 성공
}

// ===============================
// length-prefix 기반 전송 함수
// ===============================
int packet_send(int sock,               // 소켓
                const char* data,       // 실제 전송할 데이터
                uint32_t len)           // 데이터 길이
{
    uint32_t net_len = htonl(len);      // 네트워크 바이트 오더로 변환

    // 1️⃣ 먼저 4바이트 길이 전송
    if (send_all(sock,                  // 소켓
                 (char*)&net_len,       // 길이 주소
                 sizeof(net_len)) < 0)  // 4바이트 크기
        return -1;                      // 실패

    // 2️⃣ 실제 데이터 전송
    if (send_all(sock,                  // 소켓
                 data,                  // 데이터
                 len) < 0)              // 데이터 길이
        return -1;                      // 실패

    return 0;                           // 성공
}

// ===============================
// length-prefix 기반 수신 함수
// ===============================
int packet_recv(int sock,               // 소켓
                char** out_buf,         // 결과 버퍼 주소 반환
                uint32_t* out_len)      // 결과 길이 반환
{
    uint32_t net_len;                   // 네트워크 바이트 오더 길이 저장

    // 1️⃣ 먼저 4바이트 길이 수신
    if (recv_all(sock,                  // 소켓
                 (char*)&net_len,       // 길이 저장 위치
                 sizeof(net_len)) < 0)  // 4바이트 읽기
        return -1;                      // 실패

    uint32_t len = ntohl(net_len);      // 호스트 바이트 오더로 변환

    char* buf = (char*)malloc(len + 1); // JSON 문자열 저장을 위해 len+1 할당

    if (!buf)                           // 메모리 할당 실패
        return -1;                      // 실패

    // 2️⃣ 실제 데이터 수신
    if (recv_all(sock,                  // 소켓
                 buf,                   // 저장 버퍼
                 len) < 0)              // 길이만큼 읽기
    {
        free(buf);                      // 실패 시 메모리 해제
        return -1;                      // 실패
    }

    buf[len] = '\0';                    // 문자열 종료 문자 추가

    *out_buf = buf;                     // 호출자에게 버퍼 전달
    *out_len = len;                     // 호출자에게 길이 전달

    return 0;                          
}