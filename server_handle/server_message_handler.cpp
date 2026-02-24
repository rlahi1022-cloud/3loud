#include <iostream>                                                                 
#include <string>                                                                   
#include <limits>                                                                   
#include <arpa/inet.h>                                                              
#include <unistd.h>                                                                 
#include <nlohmann/json.hpp>                                                        
#include "./protocol/json_packet.hpp"                                                   
#include "./protocol/protocal.h"
#include "./protocol/protocol_schema.h"
#include "server_message_handler.hpp"

extern "C" {                                // C 모듈을 C 링크로 사용
#include "./protocol/packet.h"           // length-prefix send/recv 공용 모듈
}                                           // extern "C" 끝

using json = nlohmann::json;        

/**
 * [요구사항 반영 완료]
 * 1. Payload 내부 접근 통일: req["payload"] 구조 강제
 * 2. 세션 검증 추가: task.is_logged_in 확인을 통한 보안 강화
 * 3. 응답 통일: protocal.h의 ResultValue 상수를 사용하여 value 필드로 응답
 * 4. 구조 개선: Worker가 직접 Session Map에 접근하지 않고 Task에 복사된 정보 활용
 */
    std::string handle_msg_send(const Task& task, sql::Connection& db) {
    try {
        // 1️⃣ 클라이언트가 보낸 전체 패킷을 JSON으로 변환
        json req = json::parse(task.payload);
        
        // 2️⃣ 요구사항에 따라 'payload' 키 내부의 데이터만 추출
        json payload = req.value("payload", json::object());

        // 3️⃣ 세션 검증: 로그인하지 않은 사용자의 DB 접근 차단 (Worker 최적화 구조)
        if (!task.is_logged_in || task.session_email.empty()) {
            return make_resp(PKT_MSG_SEND_REQ, VALUE_ERR_SESSION, "세션이 만료되었습니다.", json::object());
        }

        // 4️⃣ DB에 저장할 변수들 준비
        std::string receiver = payload.value("receiver_email", ""); // 수신자
        std::string content  = payload.value("content", "");        // 메시지 내용
        json vector_data     = payload.value("content_vector", json::array()); // 벡터 데이터

        // 5️⃣ SQL 실행 객체 생성 (PreparedStatement로 SQL 인젝션 방지)
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement("INSERT INTO messages (sender_email, receiver_email, content, content_vector) VALUES (?, ?, ?, ?)")
        );

        // 6️⃣ 각 파라미터 바인딩 (순서 중요)
        pstmt->setString(1, task.session_email); // 송신자: 클라이언트 데이터가 아닌 '서버 세션' 정보 사용 (보안)
        pstmt->setString(2, receiver);           // 수신자
        pstmt->setString(3, content);            // 내용
        pstmt->setString(4, vector_data.dump()); // 벡터: JSON 배열을 문자열로 직렬화하여 저장

        // 7️⃣ 실제 DB에 데이터 삽입 실행
        pstmt->executeUpdate();

        // 8️⃣ 성공 응답 반환 (protocol.h의 VALUE_SUCCESS = 0 사용)
        return make_resp(PKT_MSG_SEND_REQ, VALUE_SUCCESS, "메시지가 DB에 안전하게 저장되었습니다.", json::object());

    } catch (const sql::SQLException& e) {
        // DB 관련 에러 발생 시 로그 출력 및 실패 응답
        std::cerr << "[DB Error] " << e.what() << std::endl;
        return make_resp(PKT_MSG_SEND_REQ, VALUE_ERR_DB, "데이터베이스 저장 실패", json::object());
    } catch (const std::exception& e) {
        // 기타 서버 로직 에러 처리
        return make_resp(PKT_MSG_SEND_REQ, VALUE_ERR_UNKNOWN, "서버 내부 오류", json::object());
    }
}

//서버 메시지 조회 핸들러

std::string handle_msg_list(const Task& task, sql::Connection& db) {
    try {
        // -----------------------------------------------------------------
        // 1. JSON 파싱 및 Payload 접근 (요구사항: Payload 중심 접근 통일)
        // -----------------------------------------------------------------
        json req = json::parse(task.payload);                   // 수신 패킷 파싱
        json payload = req.value("payload", json::object());     // 비즈니스 로직은 payload 안에 있음

        // -----------------------------------------------------------------
        // 2. 세션 검증 (요구사항: 모든 메시지 기능에 세션 검증 추가)
        // -----------------------------------------------------------------
        // Worker가 직접 sessions 맵에 Lock을 걸지 않고, Task에 복사된 정보를 활용 (구조 개선)
        if (!task.is_logged_in || task.session_email.empty()) {
            // VALUE_ERR_SESSION: protocal.h에 정의된 -3 사용
            return make_resp(PKT_MSG_LIST_REQ, VALUE_ERR_SESSION, "인증 세션이 유효하지 않습니다.", json::object());
        }

        // -----------------------------------------------------------------
        // 3. 필터링 파라미터 추출
        // -----------------------------------------------------------------
        // mode: "inbox" (수신함) 혹은 "sent" (발신함)
        std::string mode = payload.value("mode", "inbox");

        // -----------------------------------------------------------------
        // 4. SQL 쿼리 구성 (CREATE TABLE messages.txt 구조 반영)
        // -----------------------------------------------------------------
        // 보안 핵심: 본인의 메시지만 조회하도록 WHERE 절에 task.session_email을 강제 바인딩
        std::string sql;
        if (mode == "sent") {
            // 내가 보낸 메시지: sender_email이 나인 경우
            sql = "SELECT id, receiver_email AS contact, content, sent_at, is_read "
                  "FROM messages WHERE sender_email = ? ORDER BY sent_at DESC";
        } else {
            // 내가 받은 메시지: receiver_email이 나인 경우
            sql = "SELECT id, sender_email AS contact, content, sent_at, is_read "
                  "FROM messages WHERE receiver_email = ? ORDER BY sent_at DESC";
        }

        std::unique_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(sql));
        pstmt->setString(1, task.session_email); // 쿼리 매개변수에 인증된 이메일 주입

        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery()); // 쿼리 실행

        // -----------------------------------------------------------------
        // 5. DB 결과 패킹 (JSON 리스트화)
        // -----------------------------------------------------------------
        json msg_list = json::array(); // 메시지들을 담을 배열
        while (res->next()) {
            json msg_item;
            msg_item["id"]       = res->getInt("id");         // 메시지 고유 ID
            msg_item["contact"]  = res->getString("contact"); // 상대방 이메일
            msg_item["content"]  = res->getString("content"); // 메시지 내용
            msg_item["sent_at"]  = res->getString("sent_at"); // 발송 시각
            msg_item["is_read"]  = res->getInt("is_read");    // 읽음 여부
            msg_list.push_back(msg_item);                     // 배열에 추가
        }

        // -----------------------------------------------------------------
        // 6. 최종 응답 생성 (요구사항: value 필드 기준 응답 통일)
        // -----------------------------------------------------------------
        json resp_payload;
        resp_payload["messages"] = msg_list;      // 조회된 메시지 목록
        resp_payload["count"]    = msg_list.size(); // 총 건수

        // VALUE_SUCCESS: protocal.h에 정의된 0 사용
        return make_resp(PKT_MSG_LIST_REQ, VALUE_SUCCESS, "목록 조회를 성공했습니다.", resp_payload);

    } catch (const sql::SQLException& e) {
        // SQL 관련 예외 처리 (VALUE_ERR_DB: -2)
        std::cerr << "[DB Error] " << e.what() << std::endl;
        return make_resp(PKT_MSG_LIST_REQ, VALUE_ERR_DB, "DB 조회 중 오류가 발생했습니다.", json::object());
    } catch (const std::exception& e) {
        // 일반 예외 처리 (VALUE_ERR_UNKNOWN: -1)
        return make_resp(PKT_MSG_LIST_REQ, VALUE_ERR_UNKNOWN, "서버 내부 로직 오류", json::object());
    }
}


