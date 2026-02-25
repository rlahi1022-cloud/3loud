#include "message_handler.hpp"
#include "protocol.h"
#include "json_packet.hpp"
#include "server.h"
#include <memory>

using json = nlohmann::json;

// ========================================================
// 메시지 전송 (세션 기반)
// ========================================================
std::string handle_msg_send(const json& req, sql::Connection& db)
{
    try
    {
        json payload = get_payload(req);

        std::string receiver = payload.value("receiver_email", "");
        std::string content  = payload.value("content", "");
        std::string vector_str =
            payload.value("content_vector", json::array()).dump();

        if (receiver.empty() || content.empty())
        {
            json res = make_response(PKT_MSG_SEND_REQ,
                                     VALUE_ERR_INVALID_PACKET);
            res["msg"] = "필수 필드 누락";
            return res.dump();
        }

        if (content.length() > 1024)
        {
            json res = make_response(PKT_MSG_SEND_REQ,
                                     VALUE_ERR_INVALID_PACKET);
            res["msg"] = "1024바이트 초과";
            return res.dump();
        }

        // ==========================
        // ★ 세션에서 sender 가져오기
        // ==========================
        std::string sender;

        {
            std::lock_guard<std::mutex> lock(g_login_m);

            auto it = g_socket_users.find(g_current_sock);
            if (it == g_socket_users.end())
            {
                json res = make_response(PKT_MSG_SEND_REQ,
                                         VALUE_ERR_SESSION);
                res["msg"] = "로그인 세션 없음";
                return res.dump();
            }

            sender = it->second;
        }

        // ==========================
        // DB INSERT
        // ==========================
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "INSERT INTO messages "
                "(sender_email, receiver_email, content, content_vector) "
                "VALUES (?, ?, ?, ?)"
            )
        );

        pstmt->setString(1, sender);
        pstmt->setString(2, receiver);
        pstmt->setString(3, content);
        pstmt->setString(4, vector_str);
        pstmt->executeUpdate();

        json res = make_response(PKT_MSG_SEND_REQ, VALUE_SUCCESS);
        res["msg"] = "전송 성공";
        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_DB);
        res["msg"] = "DB 오류";
        return res.dump();
    }
}

// ========================================================
// 메시지 목록 조회 (세션 기반)
// ========================================================
std::string handle_msg_list(const json& req, sql::Connection& db)
{
    try
    {
        // ==========================
        // ★ 세션에서 사용자 이메일 가져오기
        // ==========================
        std::string user_email;

        {
            std::lock_guard<std::mutex> lock(g_login_m);

            auto it = g_socket_users.find(g_current_sock);
            if (it == g_socket_users.end())
            {
                json res = make_response(PKT_MSG_LIST_REQ,
                                         VALUE_ERR_SESSION);
                res["msg"] = "로그인 세션 없음";
                return res.dump();
            }

            user_email = it->second;
        }

        // ==========================
        // 메시지 조회
        // ==========================
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "SELECT id, sender_email, content, "
                "DATE_FORMAT(sent_at, '%Y-%m-%d %H:%i:%s') as sent_at "
                "FROM messages "
                "WHERE receiver_email = ? OR sender_email = ? "
                "ORDER BY sent_at DESC LIMIT 20"
            )
        );

        pstmt->setString(1, user_email);
        pstmt->setString(2, user_email);

        std::unique_ptr<sql::ResultSet> res_db(pstmt->executeQuery());

        json msg_list = json::array();

        while (res_db->next())
        {
            msg_list.push_back({
                {"id", res_db->getInt("id")},
                {"sender", res_db->getString("sender_email")},
                {"content", res_db->getString("content")},
                {"sent_at", res_db->getString("sent_at")}
            });
        }

        json res = make_response(PKT_MSG_LIST_REQ, VALUE_SUCCESS);
        res["msg"] = "목록 조회 성공";
        res["payload"]["messages"] = msg_list;

        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_MSG_LIST_REQ, VALUE_ERR_DB);
        res["msg"] = "DB 오류";
        return res.dump();
    }
}