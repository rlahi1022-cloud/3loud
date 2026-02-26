#ifndef PACKET_H                                                                    // 헤더 중복 포함 방지 시작
#define PACKET_H                                                                    // 헤더 매크로 정의

#include <stdint.h>                                                                 // uint32_t 타입 사용

#ifdef __cplusplus                                                                  // C++ 컴파일러면
extern "C" {                                                                        // C 링크로 함수 노출
#endif                                                                             

int packet_send(int sock, const char* data, uint32_t len);                          //  length-prefix 전송 API
int packet_recv(int sock, char** out_buf, uint32_t* out_len);                       // length-prefix 수신 API (malloc 버퍼 반환)

#ifdef __cplusplus                                                                  // C++ 컴파일러면
}                                                                                   // extern "C" 닫기
#endif                                                                             

#endif                                                                              