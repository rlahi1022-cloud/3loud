// ============================================================
// admin_handler.cpp
// ============================================================
#include "admin_handler.hpp"
#include "protocol.h"
#include "json_packet.hpp"

#include <mutex>
#include <unordered_map>

// skeleton_server.cpp 에 정의된 전역 변수들을 여기서 사용하겠다고 명시
extern std::mutex g_login_m;
extern std::unordered_map<std::string, int> g_login_users;

using json = nlohmann::json;

// [수정됨] inline 키워드 제거
std::string handle_admin_user_list(const json &req, sql::Connection &db)
{
    try
    {
        bool only_inactive = req.value("payload", json::object()).value("only_inactive", false);
        std::string query = "SELECT no, email, nickname, is_active FROM users";
        if (only_inactive)
            query += " WHERE is_active = 0";

        std::unique_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(query));
        std::unique_ptr<sql::ResultSet> rs(pstmt->executeQuery());

        json user_list = json::array();
        while (rs->next())
        {
            std::string email = rs->getString("email").c_str();
            bool is_online = false;

            // 접속 여부 확인
            {
                std::lock_guard<std::mutex> lock(g_login_m);
                if (g_login_users.find(email) != g_login_users.end())
                    is_online = true;
            }

            user_list.push_back({{"no", rs->getInt("no")},
                                 {"email", email},
                                 {"nickname", rs->getString("nickname").c_str()},
                                 {"is_active", rs->getInt("is_active")},
                                 {"is_online", is_online}});
        }

        json res = make_response(PKT_ADMIN_USER_LIST_REQ, VALUE_SUCCESS);
        res["payload"]["users"] = user_list;
        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_ADMIN_USER_LIST_REQ, VALUE_ERR_DB);
        res["msg"] = e.what();
        return res.dump();
    }
}

// [수정됨] inline 키워드 제거
std::string handle_admin_user_info(const json &req, sql::Connection &db)
{
    try
    {
        int target_no = req.value("payload", json::object()).value("target_no", 0);

        std::unique_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(
            "SELECT u.no, u.email, u.nickname, u.created_at, u.grade, u.is_active, "
            "IFNULL((SELECT SUM(file_size) FROM files f WHERE f.no = u.no), 0) AS storage_used "
            "FROM users u WHERE u.no = ?"));
        pstmt->setInt(1, target_no);
        std::unique_ptr<sql::ResultSet> rs(pstmt->executeQuery());

        if (!rs->next())
            return make_response(PKT_ADMIN_USER_INFO_REQ, VALUE_ERR_USER_NOT_FOUND).dump();

        json res = make_response(PKT_ADMIN_USER_INFO_REQ, VALUE_SUCCESS);
        res["payload"] = {
            {"no", rs->getInt("no")},
            {"email", rs->getString("email").c_str()},
            {"nickname", rs->getString("nickname").c_str()},
            {"created_at", rs->getString("created_at").c_str()},
            {"grade", rs->getInt("grade")},
            {"is_active", rs->getInt("is_active")},
            {"storage_used", rs->getInt64("storage_used")}};
        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_ADMIN_USER_INFO_REQ, VALUE_ERR_DB);
        res["msg"] = e.what();
        return res.dump();
    }
}

// [수정됨] inline 키워드 제거
std::string handle_admin_state_change(const json &req, sql::Connection &db)
{
    try
    {
        json payload = req.value("payload", json::object());
        int target_no = payload.value("target_no", 0);
        int is_active = payload.value("is_active", 1);

        std::unique_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(
            "UPDATE users SET is_active = ? WHERE no = ?"));
        pstmt->setInt(1, is_active);
        pstmt->setInt(2, target_no);
        pstmt->executeUpdate();

        return make_response(PKT_ADMIN_STATE_CHANGE_REQ, VALUE_SUCCESS).dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_ADMIN_STATE_CHANGE_REQ, VALUE_ERR_DB);
        res["msg"] = e.what();
        return res.dump();
    }
}