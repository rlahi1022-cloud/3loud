// ============================================================
// message_handler.cpp
// DB 스키마 기준:
//   users   : no(PK), email, pw_hash, nickname, ...
//   messages: msg_id(PK), from_email, to_user_id(→users.no),
//             content, is_read, is_temp, sent_at
//   blacklist: owner_user_id(→users.no), blocked_email
//
// 세션 맵: g_socket_users[sock] = email
// ============================================================

#include "message_handler.hpp"
#include "protocol.h"
#include "json_packet.hpp"
#include "server.h"
#include <memory>
#include <string>

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 내부 헬퍼: 현재 소켓의 세션 이메일 반환
// ──────────────────────────────────────────────
static std::string get_session_email(int sock)
{
    std::lock_guard<std::mutex> lock(g_login_m);
    auto it = g_socket_users.find(sock);
    if (it == g_socket_users.end())
        return "";
    return it->second;
}

// ──────────────────────────────────────────────
// 내부 헬퍼: 이메일 → users.no (없으면 0)
// ──────────────────────────────────────────────
static unsigned int get_user_no(sql::Connection &db,
                                const std::string &email)
{
    std::unique_ptr<sql::PreparedStatement> ps(
        db.prepareStatement("SELECT no FROM users WHERE email = ? LIMIT 1"));
    ps->setString(1, email);
    std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
    if (rs->next())
        return (unsigned int)rs->getUInt("no");
    return 0;
}

// ──────────────────────────────────────────────
// 내부 헬퍼: 블랙리스트 체크
//   수신자(receiver_no)가 송신자(sender_email)를 차단했는지
// ──────────────────────────────────────────────
// ──────────────────────────────────────────────  // 구분 주석
// 내부 헬퍼: 블랙리스트 체크 (시그니처 유지, 내부만 이메일 기반으로 정합)         // 설명
// receiver_no(=users.no) -> owner_email(users.email) 변환 후 blacklist(owner_email) 조회 // 설명
// ──────────────────────────────────────────────  // 구분 주석
static bool is_blacklisted(sql::Connection &db,             // DB 커넥션
                           unsigned int receiver_no,        // 수신자 user_no (시그니처 유지)
                           const std::string &sender_email) // 송신자 이메일
{
    try // 예외 보호
    {
        // 1) receiver_no -> receiver_email 변환                                    // 변환 설명
        std::unique_ptr<sql::PreparedStatement> ps_owner(      // owner 조회 statement
            db.prepareStatement(                               // SQL 준비
                "SELECT email FROM users WHERE no = ? LIMIT 1" // user_no -> email
                ));
        ps_owner->setUInt(1, receiver_no);                                  // receiver_no 바인딩
        std::unique_ptr<sql::ResultSet> rs_owner(ps_owner->executeQuery()); // 실행

        if (!rs_owner->next()) // 사용자가 없으면
        {
            return false; // 차단 없음 처리
        }

        std::string owner_email = rs_owner->getString("email").c_str(); // owner_email 확보

        // 2) blacklist(owner_email, blocked_email) 조회                             // 조회 설명
        std::unique_ptr<sql::PreparedStatement> ps(            // blacklist 조회 statement
            db.prepareStatement(                               // SQL 준비
                "SELECT 1 FROM blacklist "                     // 존재 체크
                "WHERE owner_email = ? AND blocked_email = ? " // 조건
                "LIMIT 1"                                      // 1개면 충분
                ));
        ps->setString(1, owner_email);                          // owner_email 바인딩
        ps->setString(2, sender_email);                         // blocked_email 바인딩
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery()); // 실행

        return rs->next(); // 있으면 true
    }
    catch (...) // 예외 시
    {
        return false; // 안전하게 차단 없음 처리
    }
}

// ============================================================
// handle_msg_send  (PKT_MSG_SEND_REQ = 0x0010)
//
// 요청 payload:
//   { "to": "수신자이메일", "content": "본문" }
//
// 처리 순서:
//   1. 세션 → 송신자 이메일
//   2. payload 파싱 및 길이 검증 (최대 1024 bytes)
//   3. 수신자 이메일 → users.no  (없으면 VALUE_ERR_USER_NOT_FOUND)
//   4. 블랙리스트 체크  (차단 시 조용히 SUCCESS 반환)
//   5. DB INSERT messages
// ============================================================
// ============================================================
// handle_msg_poll  (PKT_MSG_POLL_REQ = 0x0011)
//
// 폴링 전용 핸들러: 세션 없이 email+pw_hash로 직접 인증
// 중복 로그인 체크 없이 has_unread 만 반환
//
// 요청 payload: { "email": "...", "pw_hash": "..." }
// 응답 payload: { "has_unread": true/false }
// ============================================================
std::string handle_msg_poll(const json &req, sql::Connection &db)
{
    try
    {
        json payload = get_payload(req);
        std::string email = payload.value("email", "");
        std::string pw_hash = payload.value("pw_hash", "");

        if (email.empty() || pw_hash.empty())
        {
            json res = make_response(PKT_MSG_POLL_REQ, VALUE_ERR_INVALID_PACKET);
            res["msg"] = "email/pw_hash 누락";
            return res.dump();
        }

        // pw_hash 검증
        std::unique_ptr<sql::PreparedStatement> auth_st(
            db.prepareStatement("SELECT pw_hash FROM users WHERE email = ? LIMIT 1"));
        auth_st->setString(1, email);
        std::unique_ptr<sql::ResultSet> auth_rs(auth_st->executeQuery());
        if (!auth_rs->next() ||
            std::string(auth_rs->getString("pw_hash").c_str()) != pw_hash)
        {
            json res = make_response(PKT_MSG_POLL_REQ, VALUE_ERR_INVALID_PACKET);
            res["msg"] = "인증 실패";
            return res.dump();
        }

        // has_unread 조회
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "SELECT COUNT(*) AS cnt FROM messages "
                "WHERE to_email = ? AND is_read = 0 LIMIT 1"));
        pstmt->setString(1, email);
        std::unique_ptr<sql::ResultSet> rs(pstmt->executeQuery());
        bool has_unread = false;
        if (rs->next())
            has_unread = (rs->getInt("cnt") > 0);

        json res = make_response(PKT_MSG_POLL_REQ, VALUE_SUCCESS);
        res["msg"] = "ok";
        res["payload"] = {{"has_unread", has_unread}};
        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        std::cerr << "[POLL] SQL 예외: " << e.what() << std::endl;
        json res = make_response(PKT_MSG_POLL_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (const std::exception &e)
    {
        json res = make_response(PKT_MSG_POLL_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = std::string("오류: ") + e.what();
        return res.dump();
    }
}

std::string handle_msg_send(const json &req, sql::Connection &db)
{
    try
    {
        // 1. 세션 확인
        std::string sender_email = get_session_email(g_current_sock);
        if (sender_email.empty())
        {
            json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_SESSION);
            res["msg"] = "로그인 세션 없음";
            return res.dump();
        }

        // 2. payload 파싱
        json payload = get_payload(req);
        std::string receiver_email = payload.value("to", "");
        std::string content = payload.value("content", "");

        if (receiver_email.empty() || content.empty())
        {
            json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_INVALID_PACKET);
            res["msg"] = "필수 필드(to, content) 누락";
            return res.dump();
        }

        // 길이 제한 (기본/마무리 메시지 포함 클라이언트에서 조합 후 전송)
        if (content.size() > 1024)
        {
            json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_INVALID_PACKET);
            res["msg"] = "메시지 1024 bytes 초과";
            return res.dump();
        }

        // 3. 수신자 users.no 조회
        unsigned int receiver_no = get_user_no(db, receiver_email);
        if (receiver_no == 0)
        {
            json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_USER_NOT_FOUND);
            res["msg"] = "수신자를 찾을 수 없음";
            return res.dump();
        }

        // 4. 블랙리스트 체크
        //    요구사항 13-2-3-1: "블랙리스트에 추가된 계정으로 온 메시지는 무시"
        //    → 서버에서 조용히 무시, 송신자에게는 성공으로 응답
        if (is_blacklisted(db, receiver_no, sender_email))
        {
            json res = make_response(PKT_MSG_SEND_REQ, VALUE_SUCCESS);
            res["msg"] = "전송 완료";
            return res.dump();
        }
        // =======================================================
        // [ADMIN NEW] 관리자가 보내는 메시지 앞부분에 [gm닉네임] 자동 추가
        // =======================================================
        try
        {
            // 보낸 사람의 user_no와 nickname을 DB에서 조회
            std::unique_ptr<sql::PreparedStatement> ps_sender(
                db.prepareStatement("SELECT no, nickname FROM users WHERE email = ? LIMIT 1"));
            ps_sender->setString(1, sender_email);
            std::unique_ptr<sql::ResultSet> rs_sender(ps_sender->executeQuery());

            if (rs_sender->next())
            {
                unsigned int sender_no = rs_sender->getUInt("no");
                std::string sender_nickname = rs_sender->getString("nickname").c_str();

                // 관리자 계정용 명찰 (핑크색)
                if (sender_no >= 1 && sender_no <= 4)
                {
                    // \033[95m : 밝은 핑크색 시작
                    // \033[0m  : 색상 초기화 (본문은 원래 색으로)
                    content = "\033[95m[" + sender_nickname + "]\033[0m " + content;
                }
            }
        }
        catch (...)
        {
            // 에러 발생 시 프로그램이 뻗지 않도록 무시 (원본 메시지 그대로 전송)
        }
        // =======================================================
        // 5. DB INSERT
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "INSERT INTO messages (from_email, to_email, content) "
                "VALUES (?, ?, ?)"));
        pstmt->setString(1, sender_email);
        pstmt->setString(2, receiver_email);
        pstmt->setString(3, content);
        pstmt->executeUpdate();

        json res = make_response(PKT_MSG_SEND_REQ, VALUE_SUCCESS);
        res["msg"] = "전송 완료";
        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        std::cerr << "[MSG_SEND] SQL 예외: " << e.what() << std::endl;
        json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[MSG_SEND] 예외: " << e.what() << std::endl;
        json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = std::string("오류: ") + e.what();
        return res.dump();
    }
    catch (...)
    {
        std::cerr << "[MSG_SEND] 알 수 없는 예외 발생" << std::endl;
        json res = make_response(PKT_MSG_SEND_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = "알 수 없는 오류";
        return res.dump();
    }
}

// ============================================================
// handle_msg_list  (PKT_MSG_LIST_REQ = 0x0012)
//
// 요청 payload:
//   { "page": 0 }   ← 생략 가능, 기본 0 (20개씩)
//
// 응답 payload:
//   {
//     "messages":  [{ msg_id, from_email, content, is_read, sent_at }],
//     "has_unread": true/false,
//     "page":       0
//   }
//
// - 수신 메시지 기준 (to_user_id = 내 no)
// - 최신순, 20개씩 페이징
// ============================================================
std::string handle_msg_list(const json &req, sql::Connection &db)
{
    try
    {
        // 1. 세션 확인
        std::string user_email = get_session_email(g_current_sock);
        if (user_email.empty())
        {
            json res = make_response(PKT_MSG_LIST_REQ, VALUE_ERR_SESSION);
            res["msg"] = "로그인 세션 없음";
            return res.dump();
        }

        // 2. page 처리
        json payload = get_payload(req);
        int page = payload.value("page", 0);
        if (page < 0)
            page = 0;
        int offset = page * 20;

        // 3. 블랙리스트 필터 포함 조회
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "SELECT msg_id, from_email, content, is_read, "
                "DATE_FORMAT(sent_at, '%Y-%m-%d %H:%i:%s') AS sent_at "
                "FROM messages m "
                "WHERE m.to_email = ? "
                "AND NOT EXISTS ( "
                "    SELECT 1 FROM blacklist b "
                "    WHERE b.owner_email = ? "
                "    AND b.blocked_email = m.from_email "
                ") "
                "ORDER BY m.sent_at DESC "
                "LIMIT 20 OFFSET ?"));

        pstmt->setString(1, user_email); // to_email
        pstmt->setString(2, user_email); // owner_email (블랙리스트 주인)
        pstmt->setInt(3, offset);        // 페이지 offset

        std::unique_ptr<sql::ResultSet> rs(pstmt->executeQuery());

        json msg_list = json::array();
        bool has_unread = false;

        while (rs->next())
        {
            bool is_read = (rs->getInt("is_read") != 0);
            if (!is_read)
                has_unread = true;

            msg_list.push_back({{"msg_id", (int)rs->getUInt("msg_id")},
                                {"from_email", rs->getString("from_email").c_str()},
                                {"content", rs->getString("content").c_str()},
                                {"is_read", is_read},
                                {"sent_at", rs->getString("sent_at").c_str()}});
        }

        json res = make_response(PKT_MSG_LIST_REQ, VALUE_SUCCESS);
        res["msg"] = "조회 성공";
        res["payload"] = {
            {"messages", msg_list},
            {"has_unread", has_unread},
            {"page", page}};

        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_MSG_LIST_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_MSG_LIST_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = "알 수 없는 오류";
        return res.dump();
    }
}
// ============================================================
// handle_msg_delete  (PKT_MSG_DELETE_REQ = 0x0013)
//
// 요청 payload:
//   { "msg_ids": [1, 2, 3] }   ← 복수 삭제 지원 (최대 100)
//
// 보안 규칙:
//   - 수신자(to_user_id = 내 no) 또는 송신자(from_email = 내 이메일)만 삭제 가능
// ============================================================
std::string handle_msg_delete(const json &req, sql::Connection &db)
{
    try
    {
        std::string user_email = get_session_email(g_current_sock);
        if (user_email.empty())
        {
            json res = make_response(PKT_MSG_DELETE_REQ, VALUE_ERR_SESSION);
            res["msg"] = "로그인 세션 없음";
            return res.dump();
        }

        unsigned int user_no = get_user_no(db, user_email);
        if (user_no == 0)
        {
            json res = make_response(PKT_MSG_DELETE_REQ, VALUE_ERR_DB);
            res["msg"] = "사용자 정보 없음";
            return res.dump();
        }

        json payload = get_payload(req);

        if (!payload.contains("msg_ids") || !payload["msg_ids"].is_array() || payload["msg_ids"].empty())
        {
            json res = make_response(PKT_MSG_DELETE_REQ, VALUE_ERR_INVALID_PACKET);
            res["msg"] = "msg_ids 필드 누락 또는 비어 있음";
            return res.dump();
        }

        auto &id_arr = payload["msg_ids"];
        if (id_arr.size() > 100)
        {
            json res = make_response(PKT_MSG_DELETE_REQ, VALUE_ERR_INVALID_PACKET);
            res["msg"] = "한 번에 최대 100개까지 삭제 가능";
            return res.dump();
        }

        // 수신자 또는 송신자 본인 메시지만 삭제
        std::unique_ptr<sql::PreparedStatement> del_stmt(
            db.prepareStatement(
                "DELETE FROM messages "
                "WHERE msg_id = ? "
                "AND (to_email = ? OR from_email = ?)"));

        int deleted_count = 0;
        json failed_ids = json::array();

        for (const auto &id_val : id_arr)
        {
            if (!id_val.is_number_integer())
                continue;

            int msg_id = id_val.get<int>();
            del_stmt->setInt(1, msg_id);
            del_stmt->setString(2, user_email);
            del_stmt->setString(3, user_email);

            int affected = del_stmt->executeUpdate();
            if (affected > 0)
                deleted_count++;
            else
                failed_ids.push_back(msg_id);
        }

        if (deleted_count == 0 && !failed_ids.empty())
        {
            json res = make_response(PKT_MSG_DELETE_REQ, VALUE_ERR_PERMISSION);
            res["msg"] = "삭제 가능한 메시지 없음 (없는 ID 또는 권한 부족)";
            res["payload"] = {{"failed_ids", failed_ids}};
            return res.dump();
        }

        json res = make_response(PKT_MSG_DELETE_REQ, VALUE_SUCCESS);
        res["msg"] = std::to_string(deleted_count) + "개 삭제 완료";
        res["payload"] = {
            {"deleted_count", deleted_count},
            {"failed_ids", failed_ids}};
        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_MSG_DELETE_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_MSG_DELETE_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = "알 수 없는 오류";
        return res.dump();
    }
}

// ============================================================
// handle_msg_read  (PKT_MSG_READ_REQ = 0x0014)
//
// 요청 payload:  { "msg_id": 123 }
//
// - 본인 수신 메시지(to_user_id = 내 no)만 읽음 처리 가능
// ============================================================
std::string handle_msg_read(const json &req, sql::Connection &db)
{
    try
    {
        std::string user_email = get_session_email(g_current_sock);
        if (user_email.empty())
        {
            json res = make_response(PKT_MSG_READ_REQ, VALUE_ERR_SESSION);
            res["msg"] = "로그인 세션 없음";
            return res.dump();
        }

        unsigned int user_no = get_user_no(db, user_email);
        if (user_no == 0)
        {
            json res = make_response(PKT_MSG_READ_REQ, VALUE_ERR_DB);
            res["msg"] = "사용자 정보 없음";
            return res.dump();
        }

        json payload = get_payload(req);
        if (!payload.contains("msg_id") || !payload["msg_id"].is_number_integer())
        {
            json res = make_response(PKT_MSG_READ_REQ, VALUE_ERR_INVALID_PACKET);
            res["msg"] = "msg_id 필드 누락";
            return res.dump();
        }

        int msg_id = payload["msg_id"].get<int>();

        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "UPDATE messages SET is_read = 1 "
                "WHERE msg_id = ? AND to_email = ?"));
        pstmt->setInt(1, msg_id);
        pstmt->setString(2, user_email);

        int affected = pstmt->executeUpdate();
        if (affected == 0)
        {
            json res = make_response(PKT_MSG_READ_REQ, VALUE_ERR_MSG_NOT_FOUND);
            res["msg"] = "메시지 없음 또는 권한 없음";
            return res.dump();
        }

        json res = make_response(PKT_MSG_READ_REQ, VALUE_SUCCESS);
        res["msg"] = "읽음 처리 완료";
        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_MSG_READ_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_MSG_READ_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = "알 수 없는 오류";
        return res.dump();
    }
}
// ============================================================
// handle_msg_setting_get
// PKT_MSG_SETTING_GET_REQ = 0x0015
// ============================================================
std::string handle_msg_setting_get(const json &req, sql::Connection &db)
{
    try
    {
        // 1. 세션 확인
        std::string email = get_session_email(g_current_sock);
        if (email.empty())
        {
            json res = make_response(PKT_MSG_SETTING_GET_REQ, VALUE_ERR_SESSION);
            res["msg"] = "로그인 세션 없음";
            return res.dump();
        }

        // 2. user_no 조회
        unsigned int user_no = get_user_no(db, email);
        if (user_no == 0)
        {
            json res = make_response(PKT_MSG_SETTING_GET_REQ, VALUE_ERR_DB);
            res["msg"] = "사용자 정보 없음";
            return res.dump();
        }

        // 3. DB 조회
        std::unique_ptr<sql::PreparedStatement> ps(
            db.prepareStatement(
                "SELECT prefix, suffix "
                "FROM message_settings "
                "WHERE user_no = ? LIMIT 1"));
        ps->setUInt(1, user_no);

        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());

        std::string prefix = "";
        std::string suffix = "";

        if (rs->next())
        {
            if (!rs->isNull("prefix"))
                prefix = rs->getString("prefix");

            if (!rs->isNull("suffix"))
                suffix = rs->getString("suffix");
        }

        // 4. 응답
        json res = make_response(PKT_MSG_SETTING_GET_REQ, VALUE_SUCCESS);
        res["msg"] = "조회 성공";
        res["payload"] = {
            {"prefix", prefix},
            {"suffix", suffix}};

        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_MSG_SETTING_GET_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_MSG_SETTING_GET_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = "알 수 없는 오류";
        return res.dump();
    }
}
// ============================================================
// handle_msg_setting_update
// PKT_MSG_SETTING_UPDATE_REQ = 0x0016
// payload:
//   { "prefix": "...", "suffix": "..." }
// ============================================================
std::string handle_msg_setting_update(const json &req, sql::Connection &db)
{
    try
    {
        // 1. 세션 확인
        std::string email = get_session_email(g_current_sock);
        if (email.empty())
        {
            json res = make_response(PKT_MSG_SETTING_UPDATE_REQ, VALUE_ERR_SESSION);
            res["msg"] = "로그인 세션 없음";
            return res.dump();
        }

        // 2. user_no 조회
        unsigned int user_no = get_user_no(db, email);
        if (user_no == 0)
        {
            json res = make_response(PKT_MSG_SETTING_UPDATE_REQ, VALUE_ERR_DB);
            res["msg"] = "사용자 정보 없음";
            return res.dump();
        }

        // 3. payload 파싱
        json payload = get_payload(req);

        std::string prefix = payload.value("prefix", "");
        std::string suffix = payload.value("suffix", "");

        // 4. UPSERT
        std::unique_ptr<sql::PreparedStatement> ps(
            db.prepareStatement(
                "INSERT INTO message_settings (user_no, prefix, suffix) "
                "VALUES (?, ?, ?) "
                "ON DUPLICATE KEY UPDATE "
                "prefix = VALUES(prefix), "
                "suffix = VALUES(suffix)"));

        ps->setUInt(1, user_no);
        ps->setString(2, prefix);
        ps->setString(3, suffix);
        ps->executeUpdate();

        json res = make_response(PKT_MSG_SETTING_UPDATE_REQ, VALUE_SUCCESS);
        res["msg"] = "설정 저장 완료";
        return res.dump();
    }
    catch (const sql::SQLException &e)
    {
        json res = make_response(PKT_MSG_SETTING_UPDATE_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_MSG_SETTING_UPDATE_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = "알 수 없는 오류";
        return res.dump();
    }
}