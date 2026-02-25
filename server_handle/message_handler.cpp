#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <mysql_connection.h>
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>

#include "protocol.h"
#include "json_packet.hpp"
using json = nlohmann::json;

std::string handle_msg_send(const json& req, sql::Connection* db, const std::string& session_email, bool is_logged_in) {
    try {
        json payload = get_payload(req);

        if (!is_logged_in || session_email.empty()) {
            return make_response(PKT_MSG_SEND_REQ, VALUE_ERR_SESSION).dump();
        }

        std::string receiver = payload.value("receiver_email", "");
        std::string content  = payload.value("content", "");
        
        // MariaDB JSON 컬럼 대응: 벡터 데이터를 JSON 문자열로 직렬화
        // std::vector 명칭 충돌 방지를 위해 nlohmann::json 명시
        std::string vector_str = payload.value("content_vector", nlohmann::json::array()).dump();

        if (content.length() > 1024) {
            return make_response(PKT_MSG_SEND_REQ, VALUE_ERR_INVALID_PACKET).dump();
        }

        std::unique_ptr<sql::PreparedStatement> pstmt(db->prepareStatement(
            "INSERT INTO messages (sender_email, receiver_email, content, content_vector) VALUES (?, ?, ?, ?)"
        ));
        pstmt->setString(1, session_email);
        pstmt->setString(2, receiver);
        pstmt->setString(3, content);
        pstmt->setString(4, vector_str);
        pstmt->executeUpdate();

        return make_response(PKT_MSG_SEND_REQ, VALUE_SUCCESS).dump();

    } catch (const std::exception& e) {
        return make_response(PKT_MSG_SEND_REQ, VALUE_ERR_DB).dump();
    }
}

std::string handle_msg_list(const json& req, sql::Connection* db, const std::string& session_email, bool is_logged_in) {
    try {
        if (!is_logged_in) {
            return make_response(PKT_MSG_LIST_REQ, VALUE_ERR_SESSION).dump();
        }

        // [지침 준수] 최근 20개만 표현되도록 제한
        std::unique_ptr<sql::PreparedStatement> pstmt(db->prepareStatement(
            "SELECT id, sender_email, content, DATE_FORMAT(sent_at, '%Y-%m-%d %H:%i:%s') as sent_at, is_read "
            "FROM messages WHERE receiver_email = ? OR sender_email = ? "
            "ORDER BY sent_at DESC LIMIT 20"
        ));
        pstmt->setString(1, session_email);
        pstmt->setString(2, session_email);
        
        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
        nlohmann::json msg_list = nlohmann::json::array();
        
        while (res->next()) {
            msg_list.push_back({
                {"id", res->getInt("id")},
                {"sender", res->getString("sender_email")},
                {"content", res->getString("content")},
                {"sent_at", res->getString("sent_at")},
                {"is_read", res->getInt("is_read")}
            });
        }

        json resp = make_response(PKT_MSG_LIST_REQ, VALUE_SUCCESS);
        resp["payload"]["messages"] = msg_list;
        return resp.dump();

    } catch (const std::exception& e) {
        return make_response(PKT_MSG_LIST_REQ, VALUE_ERR_DB).dump();
    }
}