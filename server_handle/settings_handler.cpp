// ============================================================================
// 파일명: settings_handler.cpp
// 목적: 클라이언트 파일설정 메뉴 서버 핸들러 구현
//
// PKT_SETTINGS_GET_REQ (0x0030): 용량 정보 조회
// PKT_SETTINGS_SET_REQ (0x0031): 폴더 생성 / 폴더 삭제
// ============================================================================

#include "settings_handler.hpp"
#include "file_handler.hpp"           // g_cloud_root extern 선언 포함
#include "protocol.h"     // PKT_SETTINGS_*, VALUE_*

#include <filesystem>
#include <iostream>
#include <cstring>

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 기존 skeleton_server.cpp 의 make_resp 와 동일한 형식
// ─────────────────────────────────────────────────────────────────
static std::string make_resp(int type, int code,
                             const std::string& msg,
                             const json& payload = json::object())
{
    json res;
    res["type"]    = type;           // 패킷 타입
    res["code"]    = code;           // 결과 코드
    res["msg"]     = msg;            // 결과 메시지
    res["payload"] = payload;        // 추가 데이터
    return res.dump();               // JSON 문자열로 직렬화
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: user_no 유저의 등급별 최대 허용 용량 조회 (grades 테이블)
// ─────────────────────────────────────────────────────────────────
static int64_t get_storage_total(uint32_t uno, sql::Connection& db)
{
    try {
        // grades 테이블과 JOIN 해서 등급별 최대 용량 조회
        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
            "SELECT g.max_filesize "
            "FROM users u JOIN grades g ON u.grade = g.grade "
            "WHERE u.no = ?"));
        ps->setInt(1, (int)uno);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (rs->next()) return rs->getInt64(1);    // 조회된 최대 용량 반환
    } catch (...) {}

    return 104857600LL; // 조회 실패 시 기본값 100MB
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: user_no 유저의 현재 사용 용량 조회 (users.storage_used)
// ─────────────────────────────────────────────────────────────────
static int64_t get_storage_used(uint32_t uno, sql::Connection& db)
{
    try {
        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
            "SELECT storage_used FROM users WHERE no = ?"));
        ps->setInt(1, (int)uno);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (rs->next()) return rs->getInt64(1);    // 현재 사용 용량 반환
    } catch (...) {}

    return 0; // 조회 실패 시 0 반환
}

// ─────────────────────────────────────────────────────────────────
//  PKT_SETTINGS_GET_REQ (0x0030): 설정 조회 핸들러
//  현재 지원 query: "storage" → 용량 정보 반환
// ─────────────────────────────────────────────────────────────────
std::string handle_settings_get(const json& req, sql::Connection& db)
{
    json        pl    = req.value("payload", json::object()); // 요청 payload 추출
    uint32_t    uno   = req.value("user_no", (uint32_t)0);    // 유저 번호 추출
    std::string query = pl.value("query", "");                // 조회 대상 ("storage" 등)

    if (uno == 0)                                             // 유저 번호 없으면 오류
        return make_resp(PKT_SETTINGS_GET_REQ, VALUE_ERR_INVALID_PACKET, "user_no 누락");

    // ── "storage" 쿼리: 용량 정보 반환 ──────────────────────────
    if (query == "storage" || query.empty()) {                // query 가 storage 이거나 비어있으면
        int64_t total = get_storage_total(uno, db);           // 등급별 최대 용량 조회
        int64_t used  = get_storage_used(uno, db);            // 현재 사용 용량 조회

        json ep;
        ep["storage_used"]  = used;                           // 사용 용량
        ep["storage_total"] = total;                          // 전체 허용 용량
        ep["storage_free"]  = total - used;                   // 남은 용량 미리 계산

        std::cout << "[Settings] GET storage user=" << uno
                  << " used=" << used << " total=" << total << "\n"; // 서버 로그

        return make_resp(PKT_SETTINGS_GET_REQ, VALUE_SUCCESS, "용량 조회 성공", ep);
    }

    return make_resp(PKT_SETTINGS_GET_REQ, VALUE_ERR_INVALID_PACKET, "알 수 없는 query");
}

// ─────────────────────────────────────────────────────────────────
//  PKT_SETTINGS_SET_REQ (0x0031): 설정 변경 핸들러
//
//  action = "create_folder":
//    payload["folder"] 이름의 폴더를 서버 파일시스템에 생성
//    (이미 존재하면 성공으로 처리)
//
//  action = "delete_folder":
//    payload["folder"] 폴더를 삭제
//    내부에 파일이 있으면 VALUE_ERR_UNKNOWN 으로 거절 (요구사항 13-3-4)
// ─────────────────────────────────────────────────────────────────
std::string handle_settings_set(const json& req, sql::Connection& db)
{
    json        pl     = req.value("payload", json::object()); // 요청 payload 추출
    uint32_t    uno    = req.value("user_no", (uint32_t)0);    // 유저 번호
    std::string action = pl.value("action",  "");              // 수행할 액션
    std::string folder = pl.value("folder",  "");              // 대상 폴더 이름

    if (uno == 0)                                              // 유저 번호 없으면 오류
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET, "user_no 누락");
    if (action.empty())                                        // action 누락 시 오류
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET, "action 누락");

    std::string user_root = g_cloud_root + "/" + std::to_string(uno); // 유저별 루트 폴더

    // ── list_folders: 파일시스템에서 폴더 목록 반환 ────────────────
    // 파일이 없는 빈 폴더도 감지할 수 있도록 DB 대신 파일시스템 직접 조회.
    // folder 필드가 불필요하므로 보안 검사 전에 처리.
    if (action == "list_folders") {
        json folders_arr = json::array();
        if (fs::is_directory(user_root)) {
            for (const auto& entry : fs::directory_iterator(user_root)) {
                if (entry.is_directory()) {
                    folders_arr.push_back(entry.path().filename().string());
                }
            }
        }
        json ep;
        ep["folders"] = folders_arr;
        std::cout << "[Settings] list_folders user=" << uno
                  << " count=" << folders_arr.size() << "\n";
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_SUCCESS, "폴더 목록 조회 완료", ep);
    }

    // create_folder / delete_folder 는 folder 필드 필수
    if (folder.empty())
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET, "folder 누락");

    // 폴더 이름 보안 검사: 경로 탈출 방지 (../ 등)
    if (folder.find("..") != std::string::npos ||
        folder.find('/') != std::string::npos  ||
        folder.find('\\') != std::string::npos) {
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET,
                         "폴더 이름에 허용되지 않는 문자가 포함되어 있습니다");
    }

    std::string target = user_root + "/" + folder;             // 대상 폴더 절대 경로

    // ── create_folder: 폴더 생성 ────────────────────────────────
    if (action == "create_folder") {
        // 이미 존재하면 성공으로 처리 (멱등성 보장)
        if (fs::is_directory(target)) {
            json ep; ep["folder"] = folder;
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_SUCCESS, "폴더가 이미 존재합니다", ep);
        }

        // 폴더 생성 시도
        std::error_code ec;
        fs::create_directories(target, ec);        // 중간 경로까지 한 번에 생성
        if (ec) {
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_UNKNOWN,
                             "폴더 생성 실패: " + ec.message());
        }

        json ep; ep["folder"] = folder;
        std::cout << "[Settings] 폴더 생성 user=" << uno << " folder=" << folder << "\n"; // 서버 로그
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_SUCCESS, "폴더 생성 완료", ep);
    }

    // ── delete_folder: 폴더 삭제 ────────────────────────────────
    if (action == "delete_folder") {
        if (!fs::is_directory(target)) {           // 존재하지 않는 폴더
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_FILE_NOT_FOUND,
                             "폴더를 찾을 수 없습니다");
        }

        // 요구사항 13-3-4: 내부에 파일이 있으면 삭제 불가
        // DB에서 해당 폴더에 속한 파일 수 확인
        int file_count = 0;
        try {
            std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
                "SELECT COUNT(*) FROM files WHERE no = ? AND file_path LIKE ?"));
            ps->setInt   (1, (int)uno);
            // target은 절대 경로 예: /cloud/1/work
            // 그 하위 파일: /cloud/1/work/% 로 매칭
            ps->setString(2, target + "/%");      // 해당 폴더 직접 하위 파일만 검색
            std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
            if (rs->next()) file_count = rs->getInt(1); // 파일 개수 가져오기
        } catch (const sql::SQLException& e) {
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_DB,
                             std::string("DB 오류: ") + e.what());
        }

        if (file_count > 0) {                      // 내부에 파일이 있으면 삭제 거절
            json ep; ep["file_count"] = file_count; // 파일 개수 정보 포함
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_UNKNOWN,
                             "폴더 안에 파일이 " + std::to_string(file_count) +
                             "개 있어 삭제할 수 없습니다. 파일을 먼저 삭제해주세요.",
                             ep);
        }

        // 파일 없는 빈 폴더만 삭제 허용
        std::error_code ec;
        fs::remove_all(target, ec);                // 폴더 삭제 (하위 빈 디렉토리 포함)
        if (ec) {
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_UNKNOWN,
                             "폴더 삭제 실패: " + ec.message());
        }

        json ep; ep["folder"] = folder;
        std::cout << "[Settings] 폴더 삭제 user=" << uno << " folder=" << folder << "\n"; // 서버 로그
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_SUCCESS, "폴더 삭제 완료", ep);
    }

    return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET,
                     "알 수 없는 action: " + action);
}
