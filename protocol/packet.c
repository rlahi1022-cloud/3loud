#ifndef PACKET_H                // 헤더 중복 포함 방지 매크로 시작
#define PACKET_H                // 헤더 중복 포함 방지 매크로 정의

#include <stdint.h>             // uint32_t 같은 고정 크기 정수형 사용을 위한 헤더

#ifdef __cplusplus              // 만약 C++에서 이 헤더를 include 한다면
extern "C" {                    // C 링크 방식으로 컴파일되도록 설정
#endif

// ===============================
// packet_send
// 역할:
//  - length-prefix 기반으로 데이터를 전송
//  - 먼저 4바이트 길이 전송 후 실제 데이터 전송
// 반환:
//  0  -> 성공
// -1  -> 실패
// ===============================
int packet_send(int sock,       // 전송할 소켓 파일 디스크립터
                const char* data, // 전송할 데이터 버퍼
                uint32_t len);    // 전송할 데이터 길이

// ===============================
// packet_recv
// 역할:
//  - length-prefix 기반으로 데이터를 수신
//  - 먼저 4바이트 길이를 읽고
//  - 그 길이만큼 정확히 수신
// 반환:
//  0  -> 성공
// -1  -> 실패
// ===============================
int packet_recv(int sock,       // 수신할 소켓 파일 디스크립터
                char** out_buf, // 수신된 데이터를 동적할당하여 반환
                uint32_t* out_len); // 수신된 데이터 길이 반환

#ifdef __cplusplus              // C++ 환경에서
}
#endif

#endif                        