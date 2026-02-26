// ============================================================================
// 파일명: file_handler.cpp
// 목적: 파일 업로드/다운로드/삭제/목록 서버 핸들러 구현
//
// 설계 원칙:
//   - 기존 skeleton_server.cpp의 make_resp / handle_* 패턴 완전 동일하게 작성
//   - packet_send 로 JSON 문자열 전송 (기존 worker 루프와 동일)
//   - 파일 실체는 파일시스템, 메타데이터만 DB 저장 (요구사항 14, 15항)
//   - 청크 전송: 64KB 단위 base64 인코딩 → JSON payload에 담아 packet_send
//   - 중복 파일명: name_1.ext, name_2.ext ... (요구사항 12-1-11항)
// ============================================================================

#include "file_handler.hpp"
#include "protocol.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>

extern "C" {
#include "../protocol/packet.h"     // packet_send / packet_recv (기존 C 모듈)
}

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────
//  전역: 서버 파일 저장 루트
// ─────────────────────────────────────────────────────────────────
std::string g_cloud_root; // static 제거 → settings_handler.cpp 에서 extern 참조 가능

void file_handler_init(const std::string& cloud_root)
{
    g_cloud_root = cloud_root;
    fs::create_directories(g_cloud_root);
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 기존 skeleton_server.cpp의 make_resp와 동일한 형식
// ─────────────────────────────────────────────────────────────────
static std::string make_resp(int type, int code,
                             const std::string& msg,
                             const json& payload = json::object())
{
    json res;
    res["type"]    = type;
    res["code"]    = code;
    res["msg"]     = msg;
    res["payload"] = payload;
    return res.dump();
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: sock에 JSON 응답을 packet_send로 전송 (다운로드 청크 전송용)
// ─────────────────────────────────────────────────────────────────
static bool send_resp(int sock, const std::string& json_str)
{
    return packet_send(sock,
                       json_str.c_str(),
                       static_cast<uint32_t>(json_str.size())) == 0;
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: base64 인코딩/디코딩 (외부 라이브러리 없이 직접 구현)
// ─────────────────────────────────────────────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned char b0 = data[i];
        unsigned char b1 = (i + 1 < len) ? data[i + 1] : 0;
        unsigned char b2 = (i + 2 < len) ? data[i + 2] : 0;
        out += B64[(b0 >> 2) & 0x3F];
        out += B64[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
        out += (i + 1 < len) ? B64[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
        out += (i + 2 < len) ? B64[b2 & 0x3F] : '=';
    }
    return out;
}

static std::vector<unsigned char> b64_decode(const std::string& s)
{
    static unsigned char inv[256];
    static bool init = false;
    if (!init) {
        memset(inv, 0xFF, sizeof(inv));
        for (int i = 0; i < 64; ++i)
            inv[(unsigned char)B64[i]] = (unsigned char)i;
        init = true;
    }
    std::vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (inv[c] == 0xFF) break;
        val  = (val << 6) + inv[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((unsigned char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 중복 파일명 처리 (요구사항 12-1-11)
//  같은 폴더에 동일 이름이 있으면 name_1.ext, name_2.ext ... 반환
// ─────────────────────────────────────────────────────────────────
static std::string resolve_filename(const std::string& dir,
                                    const std::string& filename)
{
    if (!fs::exists(fs::path(dir) / filename)) return filename;

    std::string stem = fs::path(filename).stem().string();
    std::string ext  = fs::path(filename).extension().string();
    for (int i = 1; i <= 9999; ++i) {
        std::string cand = stem + "_" + std::to_string(i) + ext;
        if (!fs::exists(fs::path(dir) / cand)) return cand;
    }
    return filename + "_dup";
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 유저 no로 등급별 최대 파일 크기 조회 (grades 테이블)
// ─────────────────────────────────────────────────────────────────
static int64_t get_max_filesize(uint32_t user_no, sql::Connection& db)
{
    try {
        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
            "SELECT g.max_filesize "
            "FROM users u JOIN grades g ON u.grade = g.grade "
            "WHERE u.no = ?"));
        ps->setInt(1, (int)user_no);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (rs->next()) return rs->getInt64(1);
    } catch (...) {}

    // grades 테이블 없을 경우 fallback: 등급표 하드코딩
    // 0=관리자 1=일반(100MB) 2=비지니스(200MB) 3=VIP(500MB) 4=VVIP(1GB)
    try {
        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
            "SELECT grade FROM users WHERE no = ?"));
        ps->setInt(1, (int)user_no);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (rs->next()) {
            static const int64_t limits[] = {
                1073741824LL,   // 관리자 1GB
                 104857600LL,   // 일반   100MB
                 209715200LL,   // 비지니스 200MB
                 524288000LL,   // VIP 500MB
                1073741824LL    // VVIP 1GB
            };
            int g = rs->getInt(1);
            if (g >= 0 && g <= 4) return limits[g];
        }
    } catch (...) {}

    return 104857600LL; // 기본 100MB
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 남은 클라우드 용량 = 등급 총 용량 - storage_used
// ─────────────────────────────────────────────────────────────────
static int64_t get_remaining_quota(uint32_t user_no, sql::Connection& db)
{
    try {
        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
            "SELECT g.max_filesize, u.storage_used "
            "FROM users u JOIN grades g ON u.grade = g.grade "
            "WHERE u.no = ?"));
        ps->setInt(1, (int)user_no);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (rs->next()) return rs->getInt64(1) - rs->getInt64(2);
    } catch (...) {}
    return -1;
}

// ─────────────────────────────────────────────────────────────────
//  0x0020  업로드 요청 핸들러
//
//  req payload: { "file_name": str, "file_size": int64, "folder": str,
//                 "user_no": int }
//
//  응답 (code=0 성공):
//    { "type": 0x0020, "code": 0, "msg": "ok",
//      "payload": { "resolved_name": str, "total_chunks": int64 } }
//
//  클라이언트는 READY 응답 수신 후 PKT_FILE_CHUNK를 total_chunks 번 전송
// ─────────────────────────────────────────────────────────────────
std::string handle_file_upload_req(const json& req, sql::Connection& db)
{
    json pl          = req.value("payload", json::object());
    std::string name = pl.value("file_name", "");
    int64_t     size = pl.value("file_size", (int64_t)0);
    std::string fold = pl.value("folder",    "");
    uint32_t    uno  = req.value("user_no",  (uint32_t)0);

    if (name.empty() || size <= 0 || uno == 0)
        return make_resp(PKT_FILE_UPLOAD_REQ, VALUE_ERR_INVALID_PACKET, "필수 필드 누락");

    // 등급별 파일 크기 제한
    int64_t max_size = get_max_filesize(uno, db);
    if (size > max_size) {
        json ep;
        ep["max_filesize"] = max_size;
        ep["file_size"]    = size;
        return make_resp(PKT_FILE_UPLOAD_REQ, VALUE_ERR_FILE_SIZE_LIMIT,
                         "등급별 파일 크기 초과", ep);
    }

    // 남은 용량 확인
    int64_t remaining = get_remaining_quota(uno, db);
    if (remaining < 0)
        return make_resp(PKT_FILE_UPLOAD_REQ, VALUE_ERR_DB, "DB 오류");
    if (size > remaining) {
        json ep;
        ep["remaining"] = remaining;
        ep["file_size"] = size;
        return make_resp(PKT_FILE_UPLOAD_REQ, VALUE_ERR_FILE_QUOTA_EXCEEDED,
                         "클라우드 용량 초과", ep);
    }

    // 저장 폴더 생성
    std::string save_dir = g_cloud_root + "/" + std::to_string(uno);
    if (!fold.empty()) save_dir += "/" + fold;

    try { fs::create_directories(save_dir); }
    catch (const std::exception& e) {
        return make_resp(PKT_FILE_UPLOAD_REQ, VALUE_ERR_UNKNOWN,
                         std::string("디렉토리 생성 실패: ") + e.what());
    }

    // 중복 파일명 해소
    std::string resolved = resolve_filename(save_dir, name);

    // total_chunks 계산 (64KB 단위)
    static constexpr int64_t CHUNK = 65536;
    int64_t total_chunks = (size + CHUNK - 1) / CHUNK;

    json ep;
    ep["resolved_name"] = resolved;
    ep["total_chunks"]  = total_chunks;

    std::cout << "[FileUpload] user=" << uno
              << " file=" << resolved
              << " size=" << size
              << " chunks=" << total_chunks << "\n";

    return make_resp(PKT_FILE_UPLOAD_REQ, VALUE_SUCCESS, "업로드 준비 완료", ep);
}

// ─────────────────────────────────────────────────────────────────
//  0x0021  청크 수신 핸들러
//
//  req payload: { "file_name": str,  "folder": str,
//                 "chunk_index": int, "total_chunks": int,
//                 "data_b64": str,   "file_size": int64,
//                 "user_no": int }
//
//  응답:
//    중간 청크: { "code": 0, "payload": { "chunk_index": N } }
//    마지막:   { "code": 0, "payload": { "file_id": N, "file_name": str } }
// ─────────────────────────────────────────────────────────────────
std::string handle_file_chunk(const json& req, sql::Connection& db)
{
    json        pl      = req.value("payload", json::object());
    std::string name    = pl.value("file_name",    "");
    std::string fold    = pl.value("folder",       "");
    int         cidx    = pl.value("chunk_index",  0);
    int         ctotal  = pl.value("total_chunks", 1);
    std::string b64     = pl.value("data_b64",     "");
    int64_t     fsize   = pl.value("file_size",    (int64_t)0);
    uint32_t    uno     = req.value("user_no",     (uint32_t)0);

    if (name.empty() || b64.empty() || uno == 0)
        return make_resp(PKT_FILE_CHUNK, VALUE_ERR_INVALID_PACKET, "청크 필수 필드 누락");

    // 저장 경로
    std::string save_dir = g_cloud_root + "/" + std::to_string(uno);
    if (!fold.empty()) save_dir += "/" + fold;
    std::string abs_path = save_dir + "/" + name;

    // base64 디코딩
    std::vector<unsigned char> data = b64_decode(b64);

    // 파일 쓰기: 첫 청크면 새로 생성, 이후 append
    std::ios::openmode mode = std::ios::binary | std::ios::app;
    if (cidx == 0) mode = std::ios::binary | std::ios::trunc;

    std::ofstream ofs(abs_path, mode);
    if (!ofs.is_open())
        return make_resp(PKT_FILE_CHUNK, VALUE_ERR_UNKNOWN,
                         "파일 열기 실패: " + abs_path);

    ofs.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    ofs.close();

    std::cout << "[FileChunk] user=" << uno
              << " file=" << name
              << " [" << cidx + 1 << "/" << ctotal << "]\n";

    // 마지막 청크: DB INSERT + storage_used 갱신
    bool is_last = (cidx == ctotal - 1);
    if (!is_last) {
        json ep;
        ep["chunk_index"] = cidx;
        return make_resp(PKT_FILE_CHUNK, VALUE_SUCCESS, "청크 수신", ep);
    }

    try {
        // files 테이블 INSERT
        std::unique_ptr<sql::PreparedStatement> ins(db.prepareStatement(
            "INSERT INTO files (file_name, file_size, file_path, no) "
            "VALUES (?, ?, ?, ?)"));
        ins->setString(1, std::string(name));
        ins->setInt64 (2, fsize);
        ins->setString(3, abs_path);
        ins->setInt   (4, (int)uno);
        ins->executeUpdate();

        std::unique_ptr<sql::Statement> st(db.createStatement());
        std::unique_ptr<sql::ResultSet> rs(st->executeQuery("SELECT LAST_INSERT_ID()"));
        int64_t file_id = rs->next() ? rs->getInt64(1) : -1;

        // storage_used 갱신
        std::unique_ptr<sql::PreparedStatement> upd(db.prepareStatement(
            "UPDATE users SET storage_used = storage_used + ? WHERE no = ?"));
        upd->setInt64(1, fsize);
        upd->setInt  (2, (int)uno);
        upd->executeUpdate();

        json ep;
        ep["file_id"]   = file_id;
        ep["file_name"] = name;
        ep["file_size"] = fsize;

        std::cout << "[FileChunk] 완료 file_id=" << file_id << "\n";
        return make_resp(PKT_FILE_CHUNK, VALUE_SUCCESS, "파일 업로드 완료", ep);

    } catch (const sql::SQLException& e) {
        fs::remove(abs_path); // 파일시스템 롤백
        return make_resp(PKT_FILE_CHUNK, VALUE_ERR_DB,
                         std::string("DB 오류: ") + e.what());
    }
}

// ─────────────────────────────────────────────────────────────────
//  0x0022  다운로드 요청 핸들러
//
//  req payload: { "file_id": int64, "user_no": int }
//
//  흐름:
//    1) META 응답 → packet_send (sock)
//    2) 청크 × total_chunks → packet_send (sock)
//    3) DONE 응답 반환 (worker 루프가 마지막으로 보냄)
// ─────────────────────────────────────────────────────────────────
std::string handle_file_download_req(int sock, const json& req, sql::Connection& db)
{
    json    pl      = req.value("payload", json::object());
    int64_t file_id = pl.value("file_id",  (int64_t)0);
    uint32_t uno    = req.value("user_no", (uint32_t)0);

    if (file_id <= 0 || uno == 0)
        return make_resp(PKT_FILE_DOWNLOAD_REQ, VALUE_ERR_INVALID_PACKET, "file_id 누락");

    // DB에서 파일 메타 조회 (소유권 확인 포함)
    std::string file_name, abs_path;
    int64_t     file_size = 0;
    try {
        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
            "SELECT file_name, file_size, file_path "
            "FROM files WHERE file_id = ? AND no = ?"));
        ps->setInt64(1, file_id);
        ps->setInt  (2, (int)uno);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (!rs->next())
            return make_resp(PKT_FILE_DOWNLOAD_REQ, VALUE_ERR_FILE_NOT_FOUND,
                             "파일을 찾을 수 없습니다");
        file_name = rs->getString("file_name").c_str();
        file_size = rs->getInt64("file_size");
        abs_path  = rs->getString("file_path").c_str();
    } catch (const sql::SQLException& e) {
        return make_resp(PKT_FILE_DOWNLOAD_REQ, VALUE_ERR_DB,
                         std::string("DB 오류: ") + e.what());
    }

    if (!fs::exists(abs_path))
        return make_resp(PKT_FILE_DOWNLOAD_REQ, VALUE_ERR_FILE_NOT_FOUND,
                         "서버 파일이 없습니다");

    static constexpr int64_t CHUNK = 65536;
    int64_t total_chunks = (file_size + CHUNK - 1) / CHUNK;

    // ── 1) META 응답 전송 ────────────────────────────────────────
    json meta_ep;
    meta_ep["file_name"]    = file_name;
    meta_ep["file_size"]    = file_size;
    meta_ep["total_chunks"] = total_chunks;
    std::string meta_resp = make_resp(PKT_FILE_DOWNLOAD_REQ, VALUE_SUCCESS,
                                      "다운로드 시작", meta_ep);
    if (!send_resp(sock, meta_resp)) return ""; // 소켓 오류

    // ── 2) 청크 전송 루프 ────────────────────────────────────────
    std::ifstream ifs(abs_path, std::ios::binary);
    if (!ifs.is_open())
        return make_resp(PKT_FILE_DOWNLOAD_REQ, VALUE_ERR_UNKNOWN, "파일 열기 실패");

    std::vector<unsigned char> buf(CHUNK);
    for (int64_t idx = 0; idx < total_chunks; ++idx) {
        ifs.read(reinterpret_cast<char*>(buf.data()), CHUNK);
        std::streamsize n = ifs.gcount();

        json chunk_ep;
        chunk_ep["chunk_index"]  = idx;
        chunk_ep["total_chunks"] = total_chunks;
        chunk_ep["data_b64"]     = b64_encode(buf.data(), (size_t)n);

        json chunk_pkt;
        chunk_pkt["type"]    = PKT_FILE_CHUNK;
        chunk_pkt["code"]    = VALUE_SUCCESS;
        chunk_pkt["msg"]     = "";
        chunk_pkt["payload"] = chunk_ep;

        if (!send_resp(sock, chunk_pkt.dump())) {
            std::cerr << "[FileDownload] 소켓 오류 idx=" << idx << "\n";
            return ""; // 소켓 끊김
        }

        std::cout << "[FileDownload] user=" << uno
                  << " file=" << file_name
                  << " [" << idx + 1 << "/" << total_chunks << "]\n";
    }

    // ── 3) DONE 응답 반환 (worker 루프가 클라이언트에 마지막으로 전송) ─
    json done_ep;
    done_ep["file_name"] = file_name;
    done_ep["file_size"] = file_size;
    return make_resp(PKT_FILE_DOWNLOAD_REQ, VALUE_SUCCESS, "다운로드 완료", done_ep);
}

// ─────────────────────────────────────────────────────────────────
//  0x0023  파일 삭제 핸들러
//
//  req payload: { "file_id": int64, "user_no": int }
// ─────────────────────────────────────────────────────────────────
std::string handle_file_delete_req(const json& req, sql::Connection& db)
{
    json    pl      = req.value("payload", json::object());
    int64_t file_id = pl.value("file_id",  (int64_t)0);
    uint32_t uno    = req.value("user_no", (uint32_t)0);

    if (file_id <= 0 || uno == 0)
        return make_resp(PKT_FILE_DELETE_REQ, VALUE_ERR_INVALID_PACKET, "file_id 누락");

    // DB에서 경로/크기 조회 (소유권 확인)
    std::string abs_path;
    int64_t     file_size = 0;
    try {
        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(
            "SELECT file_path, file_size FROM files WHERE file_id = ? AND no = ?"));
        ps->setInt64(1, file_id);
        ps->setInt  (2, (int)uno);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (!rs->next())
            return make_resp(PKT_FILE_DELETE_REQ, VALUE_ERR_FILE_NOT_FOUND,
                             "파일을 찾을 수 없습니다");
        abs_path  = rs->getString("file_path").c_str();
        file_size = rs->getInt64("file_size");
    } catch (const sql::SQLException& e) {
        return make_resp(PKT_FILE_DELETE_REQ, VALUE_ERR_DB,
                         std::string("DB 오류: ") + e.what());
    }

    // 파일시스템 삭제 (없어도 DB는 정리)
    std::error_code ec;
    fs::remove(abs_path, ec);

    // DB DELETE + storage_used 차감
    try {
        std::unique_ptr<sql::PreparedStatement> del(db.prepareStatement(
            "DELETE FROM files WHERE file_id = ? AND no = ?"));
        del->setInt64(1, file_id);
        del->setInt  (2, (int)uno);
        del->executeUpdate();

        std::unique_ptr<sql::PreparedStatement> upd(db.prepareStatement(
            "UPDATE users SET storage_used = GREATEST(0, storage_used - ?) WHERE no = ?"));
        upd->setInt64(1, file_size);
        upd->setInt  (2, (int)uno);
        upd->executeUpdate();
    } catch (const sql::SQLException& e) {
        return make_resp(PKT_FILE_DELETE_REQ, VALUE_ERR_DB,
                         std::string("DB 삭제 오류: ") + e.what());
    }

    json ep;
    ep["file_id"] = file_id;
    std::cout << "[FileDelete] user=" << uno << " file_id=" << file_id << "\n";
    return make_resp(PKT_FILE_DELETE_REQ, VALUE_SUCCESS, "파일 삭제 완료", ep);
}

// ─────────────────────────────────────────────────────────────────
//  0x0024  파일 목록 핸들러
//
//  req payload: { "folder": str, "user_no": int }
//
//  응답 payload:
//    { "files": [ { file_id, file_name, file_size, created_at, folder } ],
//      "storage_used": int64, "storage_total": int64 }
// ─────────────────────────────────────────────────────────────────
std::string handle_file_list_req(const json& req, sql::Connection& db)
{
    json        pl   = req.value("payload", json::object());
    std::string fold = pl.value("folder",   "");
    uint32_t    uno  = req.value("user_no", (uint32_t)0);

    if (uno == 0)
        return make_resp(PKT_FILE_LIST_REQ, VALUE_ERR_INVALID_PACKET, "user_no 누락");

    json    files_arr   = json::array();
    int64_t storage_used  = 0;
    int64_t storage_total = 0;

    try {
        // 파일 목록 SELECT
        // 폴더 필터: g_cloud_root/{uno}/{fold}/% 로 시작하는 경로만 매칭
        // (기존 "%" + fold + "%" 방식은 이름이 포함된 모든 경로를 잘못 매칭함)
        std::string user_prefix = g_cloud_root + "/" + std::to_string(uno) + "/";
        std::string sql_q =
            "SELECT file_id, file_name, file_size, created_at, file_path "
            "FROM files WHERE no = ? ";
        if (!fold.empty()) sql_q += "AND file_path LIKE ? ";
        sql_q += "ORDER BY created_at DESC";

        std::unique_ptr<sql::PreparedStatement> ps(db.prepareStatement(sql_q));
        ps->setInt(1, (int)uno);
        // 정확한 폴더 경로 prefix로 필터링 (예: /cloud/1/work/%)
        if (!fold.empty()) ps->setString(2, user_prefix + fold + "/%");

        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        while (rs->next()) {
            json f;
            f["file_id"]   = rs->getInt64("file_id");
            f["file_name"] = rs->getString("file_name").c_str();
            f["file_size"] = rs->getInt64("file_size");
            f["created_at"]= rs->getString("created_at").c_str();

            // file_path에서 folder 추출
            std::string path = rs->getString("file_path").c_str();
            std::string prefix = g_cloud_root + "/" + std::to_string(uno) + "/";
            std::string rel = path;
            if (rel.rfind(prefix, 0) == 0) rel = rel.substr(prefix.size());
            size_t sl = rel.rfind('/');
            f["folder"] = (sl != std::string::npos) ? rel.substr(0, sl) : "";

            files_arr.push_back(f);
        }

        // 용량 정보 조회
        std::unique_ptr<sql::PreparedStatement> ps2(db.prepareStatement(
            "SELECT u.storage_used, g.max_filesize "
            "FROM users u JOIN grades g ON u.grade = g.grade WHERE u.no = ?"));
        ps2->setInt(1, (int)uno);
        std::unique_ptr<sql::ResultSet> rs2(ps2->executeQuery());
        if (rs2->next()) {
            storage_used  = rs2->getInt64(1);
            storage_total = rs2->getInt64(2);
        }
    } catch (const sql::SQLException& e) {
        return make_resp(PKT_FILE_LIST_REQ, VALUE_ERR_DB,
                         std::string("DB 오류: ") + e.what());
    }

    json ep;
    ep["files"]         = files_arr;
    ep["storage_used"]  = storage_used;
    ep["storage_total"] = storage_total;
    return make_resp(PKT_FILE_LIST_REQ, VALUE_SUCCESS, "목록 조회 완료", ep);
}
