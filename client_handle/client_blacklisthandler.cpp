#include "client_blacklisthandler.hpp"
#include "protocol.h"
#include "protocol_schema.h"
#include "json_packet.hpp"
#include <mariadb/conncpp.hpp>
#include <iostream>

/**
 * [규칙 엄수] 시그니처: std::string func(const json& req, sql::Connection& db)
 * 블랙리스트 추가 (uk_blacklist_pair 제약 조건 활용)
 */
std::string handle_blacklist_add(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    // SQL문 스키마에 정의된 owner_email과 blocked_email 추출
    std::string owner = payload.value("owner_email", "");
    std::string blocked = payload.value("blocked_email", "");

    try {
        // [테이블 숙지] INSERT INTO `blacklist` (`owner_email`, `blocked_email`)
        std::shared_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(
            "INSERT INTO blacklist (owner_email, blocked_email) VALUES (?, ?)"
        ));
        pstmt->setString(1, owner);
        pstmt->setString(2, blocked);
        pstmt->executeUpdate();

        // 성공 응답 (code: 0)
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS).dump();
    } catch (sql::SQLException& e) {
        // [테이블 숙지] UNIQUE KEY `uk_blacklist_pair` 중복 에러 처리 (1062)
        int err_code = (e.getErrorCode() == 1062) ? VALUE_ERR_ID_DUPLICATE : VALUE_ERR_DB;
        return make_optimized_response(PKT_BLACKLIST_REQ, err_code).dump();
    }
}

/**
 * [규칙 엄수] 시그니처: std::string func(const json& req, sql::Connection& db)
 * 블랙리스트 목록 조회 (idx_blocked_lookup 인덱스 활용 구조)
 */
std::string handle_blacklist_list(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    std::string owner = payload.value("owner_email", "");

    try {
        // [테이블 숙지] owner_email을 기준으로 차단 대상자 조회
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

        // 성공 시 목록을 value 필드에 담아 반환
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS, list).dump();
    } catch (sql::SQLException& e) {
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();
    }
}

/**
 * [규칙 엄수] 시그니처: std::string func(const json& req, sql::Connection& db)
 * 블랙리스트 해제 (DELETE)
 */
std::string handle_blacklist_remove(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    std::string owner = payload.value("owner_email", "");
    std::string blocked = payload.value("blocked_email", "");

    try {
        std::shared_ptr<sql::PreparedStatement> pstmt(db.prepareStatement(
            "DELETE FROM blacklist WHERE owner_email = ? AND blocked_email = ?"
        ));
        pstmt->setString(1, owner);
        pstmt->setString(2, blocked);
        pstmt->executeUpdate();

        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_SUCCESS).dump();
    } catch (sql::SQLException& e) {
        return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_DB).dump();
    }
}

/**
 * [통합 핸들러] PKT_BLACKLIST_REQ(0x0032) 신호 분기
 */
std::string handle_blacklist_process(const json& req, sql::Connection& db) {
    json payload = get_payload(req);
    std::string action = payload.value("action", "");

    if (action == "add") return handle_blacklist_add(req, db);
    if (action == "remove") return handle_blacklist_remove(req, db);
    if (action == "list") return handle_blacklist_list(req, db);

    return make_optimized_response(PKT_BLACKLIST_REQ, VALUE_ERR_INVALID_PACKET).dump();
}