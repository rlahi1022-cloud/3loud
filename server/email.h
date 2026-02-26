// email.h
#ifndef EMAIL_H
#define EMAIL_H

#include <string>

// 외부에서 호출할 함수들만 선언 (인터페이스)

// 1. 이메일 시스템 초기화 (스레드 시작)
void email_init();

// 2. 이메일 전송 요청 (큐에 넣기만 하고 바로 리턴 - 비동기)
void email_send(const std::string &to, const std::string &subject, const std::string &body);

// 3. 시스템 종료 시 정리 (선택 사항)
void email_shutdown();

#endif