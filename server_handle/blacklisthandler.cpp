#include "blacklisthandler.hpp"
#include "protocol.h"
#include "protocol_schema.h"
#include "json_packet.hpp"
#include <mariadb/conncpp.hpp>
#include <iostream>

/**
 * [규칙 엄수] 시그니처: std::string func(const json& req, sql::Connection& db)
 * 서버 측 블랙리스트 추가 (INSERT)
 */
std::string handle_server_blacklist_add(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    // [테이블 숙지] owner_email(차단자), blocked_email(피차단자)
    std::string owner = payload.value("owner_email", "");
    std::string blocked = payload.value("blocked_email", "");

    // 본인 차단 방지 로직 (서버측 검증)
    if (owner == blocked) {
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_INVALID_PACKET).dump();
    }

    try {
        // [테이블 숙지] uk_blacklist_pair 제약 조건을 통한 중복 처리
        std::shared_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(
            "INSERT INTO blacklist (owner_email, blocked_email) VALUES (?, ?)"
        ));
        pstmt->setString(1, owner);
        pstmt->setString(2, blocked);
        pstmt->executeUpdate();

        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS).dump();
    } catch (sql::SQLException& e) {
        // MySQL/MariaDB Error 1062: Duplicate entry (이미 차단된 경우)
        if (e.getErrorCode() == 1062) {
            return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_ID_DUPLICATE).dump();
        }
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();
    }
}

/**
 * [규칙 엄수] 시그니처: std::string func(const json& req, sql::Connection& db)
 * 서버 측 블랙리스트 해제 (DELETE)
 */
std::string handle_server_blacklist_remove(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    std::string owner = payload.value("owner_email", "");
    std::string blocked = payload.value("blocked_email", "");

    try {
        std::shared_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(
            "DELETE FROM blacklist WHERE owner_email = ? AND blocked_email = ?"
        ));
        pstmt->setString(1, owner);
        pstmt->setString(2, blocked);
        
        int update_count = pstmt->executeUpdate();
        if (update_count == 0) {
            // 삭제할 대상이 없는 경우
            return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_NOT_FOUND).dump();
        }

        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS).dump();
    } catch (sql::SQLException& e) {
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();
    }
}

/**
 * [규칙 엄수] 시그니처: std::string func(const json& req, sql::Connection& db)
 * 서버 측 블랙리스트 목록 조회 (SELECT)
 */
std::string handle_server_blacklist_list(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    std::string owner = payload.value("owner_email", "");

    try {
        // [테이블 숙지] idx_blocked_lookup 인덱스 효율을 고려한 조회
        std::shared_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(
            "SELECT blocked_email, created_at FROM blacklist WHERE owner_email = ?"
        ));
        pstmt->setString(1, owner);
        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

        json list = json::array();
        while (res->next()) {
            json item;
            item["blocked_email"] = (std::string)res->getString("blocked_email");
            item["created_at"] = (std::string)res->getString("created_at");
            list.push_back(item);
        }

        // [지침 준수] value 필드에 데이터 배치
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS, list).dump();
    } catch (sql::SQLException& e) {
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();
    }
}

/**
 * [통합 핸들러] PKT_BLACKLIST_REQ(0x0032) 분기
 */
std::string handle_server_blacklist_process(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    std::string action = payload.value("action", "");

    if (action == "add") return handle_server_blacklist_add(req, db);
    if (action == "remove") return handle_server_blacklist_remove(req, db);
    if (action == "list") return handle_server_blacklist_list(req, db);

    return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_INVALID_PACKET).dump();
}