#include <mariadb/conncpp.hpp>
#include "protocol.h"
#include "json_packet.hpp"

// [지침] 모든 메시지 기능에 세션 검증 추가 및 20개 제한
std::string handle_msg_list(const json& req, sql::Connection* db, const std::string& session_email, bool is_logged_in) {
    try {
        // 1. 세션 검증 [지침: 모든 메시지 기능에 세션 검증 추가]
        if (!is_logged_in || session_email.empty()) {
            return make_response(PKT_MSG_LIST_REQ, VALUE_ERR_SESSION, "Unauthorized").dump();
        }

        // 2. DB 조회 (20개 제한) [지침: 메세지 불러올 시 20개 정도로 제한]
        std::unique_ptr<sql::PreparedStatement> pstmt(db->prepareStatement(
            "SELECT id, sender_email, content, "
            "DATE_FORMAT(sent_at, '%Y-%m-%d %H:%i:%s') as sent_at, is_read "
            "FROM messages WHERE receiver_email = ? OR sender_email = ? "
            "ORDER BY sent_at DESC LIMIT 20" // [지침 준수]
        ));
        pstmt->setString(1, session_email);
        pstmt->setString(2, session_email);
        
        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
        json msg_list = json::array();
        
        while (res->next()) {
            msg_list.push_back({
                {"id", res->getInt("id")},
                {"sender", res->getString("sender_email")},
                {"content", res->getString("content")},
                {"sent_at", res->getString("sent_at")},
                {"is_read", res->getInt("is_read")}
            });
        }

        // 3. 응답 생성 (payload.code / payload.value 구조)
        return make_response(PKT_MSG_LIST_REQ, VALUE_SUCCESS, msg_list).dump();

    } catch (const sql::SQLException& e) {
        return make_response(PKT_MSG_LIST_REQ, VALUE_ERR_DB, e.what()).dump();
    }
}

// [지침] server_handle_msg_send: payload 내부 접근 및 세션 연계
std::string handle_msg_send(const json& req, sql::Connection* db, const std::string& session_email, bool is_logged_in) {
    try {
        if (!is_logged_in) return make_response(PKT_MSG_SEND_REQ, VALUE_ERR_SESSION, "Login Required").dump();

        // 1. payload 내부 접근으로 통일 [지침 준수]
        json payload = get_payload(req); 
        std::string receiver = payload.value("receiver_email", ""); // 클라이언트 코드 규격 일치
        std::string content = payload.value("content", "");

        // 1024자 제한 (DB 스키마 기준) 
        if (content.length() > 1024 || content.empty()) {
            return make_response(PKT_MSG_SEND_REQ, VALUE_ERR_INVALID_PACKET, "Content constraint error").dump();
        }

        // 2. DB INSERT 
        std::unique_ptr<sql::PreparedStatement> pstmt(db->prepareStatement(
            "INSERT INTO messages (sender_email, receiver_email, content) VALUES (?, ?, ?)"
        ));
        pstmt->setString(1, session_email);
        pstmt->setString(2, receiver);
        pstmt->setString(3, content);
        pstmt->executeUpdate();

        return make_response(PKT_MSG_SEND_REQ, VALUE_SUCCESS, "Message sent").dump();
    } catch (const std::exception& e) {
        return make_response(PKT_MSG_SEND_REQ, VALUE_ERR_DB, e.what()).dump();
    }
}