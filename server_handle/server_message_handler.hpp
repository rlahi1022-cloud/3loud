#pragma once  // 헤더 중복 포함 방지

// ============================================================================
// 파일명: server_message_handler.hpp
// 설명: 서버 메시지 기능 전용 핸들러 선언부
// 역할:
//   - 메시지 전송 (DB 저장)
//   - 메시지 목록 조회
// 주의:
//   - 실제 구현은 server_message_handler.cpp에 존재
//   - Worker Thread에서 호출됨
// ============================================================================

#include <string>
#include <memory>

// MariaDB C++ Connector
#include <mariadb/conncpp.hpp>

// Task 구조체는 서버 코어에서 정의됨
// (Task에는 payload, session_email, is_logged_in 포함)
struct Task;

// ---------------------------------------------------------------------------
// 메시지 전송 핸들러
// 기능:
//   - payload 내부 JSON 파싱
//   - 세션 검증
//   - DB INSERT 수행
//   - protocol.h 기반 응답 반환
// 반환값:
//   - length-prefix 기반 JSON 문자열 응답
// ---------------------------------------------------------------------------
std::string handle_msg_send(const Task& task, sql::Connection& db);

// ---------------------------------------------------------------------------
// 메시지 목록 조회 핸들러
// 기능:
//   - inbox / sent 모드 분기
//   - 세션 검증
//   - DB SELECT 수행
//   - JSON 배열 형태로 패킹 후 반환
// 반환값:
//   - length-prefix 기반 JSON 문자열 응답
// ---------------------------------------------------------------------------
std::string handle_msg_list(const Task& task, sql::Connection& db);