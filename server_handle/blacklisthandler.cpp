// ============================================================================ 
// 파일명: server_handle/blacklisthandler.cpp                                   
// 목적: 블랙리스트 서버 핸들러 (세션 기반 owner_email 확정)                    
// 규칙: 클라이언트에서 owner_email 받지 않음 (세션에서만 결정)                 
// ============================================================================ 

#include "blacklisthandler.hpp"                                                   // 블랙리스트 핸들러 헤더 포함
#include "server.h"                                                               // g_current_sock, g_socket_users, g_login_m 사용
#include "protocol.h"                                                             // PKT_BLACKLIST_REQ, VALUE_* 사용
#include "json_packet.hpp"                                                        // get_payload, make_optimized_response 사용
#include <mariadb/conncpp.hpp>                                                    // sql::Connection, PreparedStatement 사용
#include <nlohmann/json.hpp>                                                      // nlohmann::json 사용
#include <memory>                                                                 // smart pointer 사용
#include <string>                                                                 // std::string 사용
#include <mutex>                                                                  // std::mutex, lock_guard 사용

using json = nlohmann::json;                                                      // json 별칭 지정

// ───────────────────────────────────────────────────────────────────────────── 
// 내부 헬퍼: 현재 소켓 세션 이메일 얻기                                          
// ───────────────────────────────────────────────────────────────────────────── 
static std::string get_session_email_from_sock(int sock)                          // 세션 이메일 조회 함수
{                                                                                 
    std::lock_guard<std::mutex> lock(g_login_m);                                  // 세션 맵 보호 락
    auto it = g_socket_users.find(sock);                                          // 소켓 키로 세션 검색
    if (it == g_socket_users.end()) return "";                                    // 없으면 빈 문자열 반환
    return it->second;                                                            // 있으면 이메일 반환
}                                                                                 

// ───────────────────────────────────────────────────────────────────────────── 
// 내부 헬퍼: owner_email(세션) + blocked_email(payload) 추출                      
// ───────────────────────────────────────────────────────────────────────────── 
static bool get_owner_and_blocked(const json& req,                                // 요청 json 입력
                                 std::string& out_owner,                         // owner_email 출력
                                 std::string& out_blocked)                       // blocked_email 출력
{                                                                                 
    json payload = get_payload(req);                                              // payload 추출
    out_owner = get_session_email_from_sock(g_current_sock);                      // 세션에서 owner 확정
    out_blocked = payload.value("blocked_email", "");                             // payload에서 blocked_email 추출
    if (out_owner.empty()) return false;                                          // 세션 없으면 실패
    if (out_blocked.empty()) return false;                                        // 대상 없으면 실패
    return true;                                                                  // 정상 추출 성공
}                                                                                 

// ============================================================================  // 구분 주석
// blacklisthandler.cpp (수정본: 응답 포맷 통일 + 목록조회 안정화)                 // 파일 설명
// ============================================================================  // 구분 주석

// ... (상단 include / helper는 그대로 유지)                                      // 상단 유지 주석

std::string handle_server_blacklist_add(const json& req, sql::Connection& db)     // 블랙리스트 추가 핸들러
{
    std::string owner;                                                           // 차단자 이메일
    std::string blocked;                                                         // 피차단자 이메일

    if (!get_owner_and_blocked(req, owner, blocked))                              // 세션/입력 검증
    {
        return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_SESSION).dump(); // 세션 오류
    }

    if (owner == blocked)                                                        // 자기 자신 차단 방지
    {
        return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_INVALID_PACKET).dump(); // 잘못된 요청
    }

    try                                                                           // DB 작업
    {
        std::unique_ptr<sql::PreparedStatement> pstmt(                            // prepared statement
            db.prepareStatement(                                                  // SQL 준비
                "INSERT INTO blacklist (owner_email, blocked_email) "             // 컬럼 지정
                "VALUES (?, ?)"                                                   // 값 바인딩
            )
        );

        pstmt->setString(1, owner);                                               // owner_email 바인딩
        pstmt->setString(2, blocked);                                             // blocked_email 바인딩
        pstmt->executeUpdate();                                                   // INSERT 실행

        return make_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS).dump();  // 성공 응답
    }
    catch (sql::SQLException& e)                                                  // SQL 예외
    {
        if (e.getErrorCode() == 1062)                                             // 중복(UNIQUE) 에러
        {
            return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_ID_DUPLICATE).dump(); // 중복
        }
        return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();   // DB 오류
    }
}

std::string handle_server_blacklist_remove(const json& req, sql::Connection& db)  // 블랙리스트 해제 핸들러
{
    std::string owner;                                                           // 차단자 이메일
    std::string blocked;                                                         // 피차단자 이메일

    if (!get_owner_and_blocked(req, owner, blocked))                              // 세션/입력 검증
    {
        return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_SESSION).dump(); // 세션 오류
    }

    try                                                                           // DB 작업
    {
        std::unique_ptr<sql::PreparedStatement> pstmt(                            // prepared statement
            db.prepareStatement(                                                  // SQL 준비
                "DELETE FROM blacklist "                                          // 삭제
                "WHERE owner_email = ? AND blocked_email = ?"                     // 조건
            )
        );

        pstmt->setString(1, owner);                                               // owner_email 바인딩
        pstmt->setString(2, blocked);                                             // blocked_email 바인딩

        int affected = pstmt->executeUpdate();                                    // DELETE 실행

        if (affected == 0)                                                        // 삭제 대상 없음
        {
            return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_BLACKLIST_NOT_FOUND).dump(); // 없음
        }

        return make_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS).dump();  // 성공
    }
    catch (sql::SQLException&)                                                    // SQL 예외
    {
        return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();   // DB 오류
    }
}

std::string handle_server_blacklist_list(const json& req, sql::Connection& db)
{
    (void)req;  // 사용 안 함 (경고 방지)

    std::string owner = get_session_email_from_sock(g_current_sock);  // 세션에서 owner_email 획득

    if (owner.empty())
    {
        json res = make_response(PKT_BLACKLIST_REQ, VALUE_ERR_SESSION);
        res["msg"] = "로그인 세션 없음";
        return res.dump();
    }

    try
    {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "SELECT blocked_email, "
                "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at "
                "FROM blacklist "
                "WHERE owner_email = ? "
                "ORDER BY created_at DESC"
            )
        );

        pstmt->setString(1, owner);

        std::unique_ptr<sql::ResultSet> rs(pstmt->executeQuery());

        json list = json::array();

        while (rs->next())
        {
            list.push_back({
                {"blocked_email", rs->getString("blocked_email").c_str()},
                {"created_at",    rs->getString("created_at").c_str()}
            });
        }

        // payload 안에 배열을 넣어서 반환 (메시지 핸들러와 구조 통일)
        json res = make_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS);
        res["msg"] = "조회 성공";
        res["payload"] = {
            {"list", list}
        };

        return res.dump();
    }
    catch (const sql::SQLException& e)
    {
        json res = make_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB);
        res["msg"] = std::string("DB 오류: ") + e.what();
        return res.dump();
    }
    catch (...)
    {
        json res = make_response(PKT_BLACKLIST_REQ, VALUE_ERR_UNKNOWN);
        res["msg"] = "알 수 없는 오류";
        return res.dump();
    }
}
std::string handle_server_blacklist_process(const json& req, sql::Connection& db) // 통합 핸들러
{
    json payload = get_payload(req);                                              // payload 추출
    std::string action = payload.value("action", "");                             // action 추출

    if (action == "add")                                                          // add 분기
        return handle_server_blacklist_add(req, db);                              // add 처리

    if (action == "remove")                                                       // remove 분기
        return handle_server_blacklist_remove(req, db);                           // remove 처리

    if (action == "list")                                                         // list 분기
        return handle_server_blacklist_list(req, db);                             // list 처리

    return make_response(PKT_BLACKLIST_REQ, VALUE_ERR_INVALID_PACKET).dump(); // 잘못된 요청
}