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

/**
 * 서버 측 블랙리스트 추가 (INSERT)                                               
 * payload: { "action":"add", "blocked_email":"..." }                              // payload 설명
 */
std::string handle_server_blacklist_add(const json& req, sql::Connection& db)     // 블랙리스트 추가 핸들러
{                                                                                 
    std::string owner;                                                           // 차단자 이메일 변수
    std::string blocked;                                                         // 피차단자 이메일 변수

    if (!get_owner_and_blocked(req, owner, blocked))                              // 세션/입력 검증
    {                                                                             
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_SESSION).dump(); // 세션/입력 오류 반환
    }                                                                            

    if (owner == blocked)                                                        // 본인 차단 방지
    {                                                                             
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_INVALID_PACKET).dump(); // 잘못된 요청 반환
    }                                                                            

    try                                                                           // DB 작업 try
    {                                                                             // try 시작
        std::unique_ptr<sql::PreparedStatement> pstmt(                            // prepared statement 생성
            db.prepareStatement(                                                  // SQL 준비
                "INSERT INTO blacklist (owner_email, blocked_email) VALUES (?, ?)"// INSERT 쿼리
            )                                                                     // SQL 준비 끝
        );                                                                        // pstmt 생성 끝

        pstmt->setString(1, owner);                                               // owner_email 바인딩
        pstmt->setString(2, blocked);                                             // blocked_email 바인딩
        pstmt->executeUpdate();                                                   // INSERT 실행

        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS).dump();  // 성공 응답
    }                                                                             
    catch (sql::SQLException& e)                                                  // SQL 예외 처리
    {                                                                             
        if (e.getErrorCode() == 1062)                                             // 중복(UNIQUE) 에러 코드
        {                                                                         
            return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_ID_DUPLICATE).dump(); // 중복 응답
        }                                                                        
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();   // DB 오류 응답
    }                                                                            
}                                                                                 

/**
 * 서버 측 블랙리스트 해제 (DELETE)                                               
 * payload: { "action":"remove", "blocked_email":"..." }                           // payload 설명
 */
std::string handle_server_blacklist_remove(const json& req, sql::Connection& db)
{
    std::string owner;
    std::string blocked;

    if (!get_owner_and_blocked(req, owner, blocked))
    {
        return make_response(PKT_BLACKLIST_REQ,
                             VALUE_ERR_SESSION).dump();
    }

    try
    {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "DELETE FROM blacklist WHERE owner_email = ? AND blocked_email = ?"
            )
        );

        pstmt->setString(1, owner);
        pstmt->setString(2, blocked);

        int update_count = pstmt->executeUpdate();

        if (update_count == 0)
        {
            return make_response(PKT_BLACKLIST_REQ,
                                 VALUE_ERR_USER_NOT_FOUND).dump();
        }

        return make_response(PKT_BLACKLIST_REQ,
                             VALUE_SUCCESS).dump();
    }
    catch (sql::SQLException&)
    {
        return make_response(PKT_BLACKLIST_REQ,
                             VALUE_ERR_DB).dump();
    }
}                                                                  

/**
 * 서버 측 블랙리스트 목록 조회 (SELECT)                                          
 * payload: { "action":"list" }                                                    // payload 설명
 */
std::string handle_server_blacklist_list(const json& req, sql::Connection& db)
{
    std::string owner = get_session_email_from_sock(g_current_sock);
    if (owner.empty())
    {
        return make_response(PKT_BLACKLIST_REQ,
                             VALUE_ERR_SESSION).dump();
    }

    try
    {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            db.prepareStatement(
                "SELECT blocked_email, created_at FROM blacklist WHERE owner_email = ?"
            )
        );

        pstmt->setString(1, owner);
        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

        json list = json::array();

        while (res->next())
        {
            list.push_back({
                {"blocked_email", res->getString("blocked_email").c_str()},
                {"created_at",    res->getString("created_at").c_str()}
            });
        }

        json response = make_response(PKT_BLACKLIST_REQ,
                                      VALUE_SUCCESS);

        response["payload"] = list;

        return response.dump();
    }
    catch (sql::SQLException&)
    {
        return make_response(PKT_BLACKLIST_REQ,
                             VALUE_ERR_DB).dump();
    }
}
/**
 * 통합 핸들러: PKT_BLACKLIST_REQ 분기                                             
 * payload.action: "add" | "remove" | "list"                                      
 */
std::string handle_server_blacklist_process(const json& req, sql::Connection& db)
{
    json payload = get_payload(req);
    std::string action = payload.value("action", "");

    if (action == "add")
        return handle_server_blacklist_add(req, db);

    if (action == "remove")
        return handle_server_blacklist_remove(req, db);

    if (action == "list")
        return handle_server_blacklist_list(req, db);

    return make_response(PKT_BLACKLIST_REQ,
                         VALUE_ERR_INVALID_PACKET).dump();
}