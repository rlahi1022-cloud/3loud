// ============================================================================
// 파일명: file_client.cpp
// 목적: 파일 업로드/다운로드/삭제/목록 클라이언트 핸들러 구현
//
// 통신 방식:
//   - send_json / recv_json (client_net.cpp) 사용 → 기존 코드와 동일
//   - 업로드: handle_file_upload_req(0x0020) → 청크 전송(0x0021) 멀티스레드
//   - 다운로드: handle_file_download_req(0x0022) → 청크 수신
//   - 삭제: handle_file_delete_req(0x0023)
//   - 목록: handle_file_list_req(0x0024)
//
// 기존 코드에서 교체한 것:
//   - client_commented.cpp의 uploadFile/downloadFile/listFiles 함수를
//     패킷 기반으로 재구현
//   - 별도 소켓 연결 제거 → 기존 sock 하나로 모든 통신
// ============================================================================

#include "file_client.hpp"
#include "../client/client_net.hpp"
#include "../protocol/json_packet.hpp"
#include "../protocol/protocal.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────
//  전역 정의
// ─────────────────────────────────────────────────────────────────
std::atomic<bool> g_file_transfer_in_progress(false);   // 전송 중 플래그
uint32_t g_user_no = 0;                                  // 로그인 후 설정

// ─────────────────────────────────────────────────────────────────
//  기본 경로 (최초 호출 시 초기화)
//  12-1-2: 기본 업로드 검색 폴더 = ~/Downloads (실행파일 위치 제외)
//  13-3-3: 기본 다운로드 저장 폴더 = ~/Downloads
// ─────────────────────────────────────────────────────────────────
static std::string g_upload_dir;
static std::string g_download_dir;

static void init_dirs()
{
    if (!g_upload_dir.empty()) return;
    const char* home = getenv("HOME");
    std::string h    = home ? home : "/tmp";
    g_upload_dir   = h + "/Downloads";
    g_download_dir = h + "/Downloads";
    fs::create_directories(g_upload_dir);
    fs::create_directories(g_download_dir);
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: 파일 크기 사람이 읽기 좋은 문자열로 변환
// ─────────────────────────────────────────────────────────────────
static std::string human_size(int64_t b)
{
    if (b < 1024)              return std::to_string(b) + " B";
    if (b < 1024*1024)         return std::to_string(b/1024) + " KB";
    if (b < 1024*1024*1024LL)  return std::to_string(b/(1024*1024)) + " MB";
    return std::to_string(b/(1024*1024*1024LL)) + " GB";
}

// ─────────────────────────────────────────────────────────────────
//  내부 유틸: base64 인코딩/디코딩 (서버와 동일)
// ─────────────────────────────────────────────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned char b0 = data[i];
        unsigned char b1 = (i+1 < len) ? data[i+1] : 0;
        unsigned char b2 = (i+2 < len) ? data[i+2] : 0;
        out += B64[(b0 >> 2) & 0x3F];
        out += B64[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
        out += (i+1 < len) ? B64[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
        out += (i+2 < len) ? B64[b2 & 0x3F] : '=';
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
//  내부: 실제 업로드 수행 (std::thread에서 호출)
//  12-1-5: 멀티스레드로 전송 → 다른 서비스 이용 가능
//  12-1-6: g_file_transfer_in_progress = true 동안 중복 불가
//  12-1-7, 12-1-9: 완료 메시지 출력
// ─────────────────────────────────────────────────────────────────
static void upload_thread(int sock, const std::string& abs_path,
                          const std::string& file_name, const std::string& folder)
{
    // 파일 크기 확인
    std::error_code ec;
    int64_t fsize = (int64_t)fs::file_size(abs_path, ec);
    if (ec) {
        std::cout << "\n[파일 오류] 파일 크기 읽기 실패: " << ec.message() << "\n";
        g_file_transfer_in_progress = false;
        return;
    }

    // ── 0x0020 업로드 요청 ──────────────────────────────────────
    json req = make_request(PKT_FILE_UPLOAD_REQ);
    req["user_no"]         = g_user_no;
    req["payload"]["file_name"] = file_name;
    req["payload"]["file_size"] = fsize;
    req["payload"]["folder"]    = folder;

    if (!send_json(sock, req)) {
        std::cout << "\n[파일 오류] 업로드 요청 전송 실패\n";
        g_file_transfer_in_progress = false;
        return;
    }

    // READY 응답 수신
    json resp;
    if (!recv_json(sock, resp)) {
        std::cout << "\n[파일 오류] 서버 응답 수신 실패\n";
        g_file_transfer_in_progress = false;
        return;
    }

    if (resp.value("code", -1) != VALUE_SUCCESS) {
        std::cout << "\n[파일 오류] 서버 거절: " << resp.value("msg", "") << "\n";
        json& ep = resp["payload"];
        if (ep.contains("max_filesize"))
            std::cout << "  등급 허용 크기: " << human_size(ep["max_filesize"].get<int64_t>()) << "\n";
        if (ep.contains("remaining"))
            std::cout << "  남은 용량: " << human_size(ep["remaining"].get<int64_t>()) << "\n";
        g_file_transfer_in_progress = false;
        return;
    }

    json& rp        = resp["payload"];
    std::string resolved  = rp.value("resolved_name", file_name);
    int64_t total_chunks  = rp.value("total_chunks",  (int64_t)1);

    std::cout << "\n[파일 저장 중] " << resolved
              << " (" << human_size(fsize) << ")\n";

    // ── 0x0021 청크 전송 루프 ───────────────────────────────────
    static constexpr int64_t CHUNK = 65536;
    std::ifstream ifs(abs_path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cout << "\n[파일 오류] 로컬 파일 열기 실패\n";
        g_file_transfer_in_progress = false;
        return;
    }

    std::vector<unsigned char> buf(CHUNK);
    bool success = true;

    for (int64_t idx = 0; idx < total_chunks; ++idx) {
        ifs.read(reinterpret_cast<char*>(buf.data()), CHUNK);
        std::streamsize n = ifs.gcount();

        json chunk = make_request(PKT_FILE_CHUNK);
        chunk["user_no"]                  = g_user_no;
        chunk["payload"]["file_name"]     = resolved;
        chunk["payload"]["folder"]        = folder;
        chunk["payload"]["chunk_index"]   = idx;
        chunk["payload"]["total_chunks"]  = total_chunks;
        chunk["payload"]["data_b64"]      = b64_encode(buf.data(), (size_t)n);
        chunk["payload"]["file_size"]     = fsize;

        if (!send_json(sock, chunk)) {
            std::cout << "\n[파일 오류] 청크 전송 실패 ("
                      << idx+1 << "/" << total_chunks << ")\n";
            success = false;
            break;
        }

        json ack;
        if (!recv_json(sock, ack)) {
            std::cout << "\n[파일 오류] ACK 수신 실패\n";
            success = false;
            break;
        }

        if (ack.value("code", -1) != VALUE_SUCCESS) {
            std::cout << "\n[파일 오류] " << ack.value("msg", "") << "\n";
            success = false;
            break;
        }

        // 진행률 표시 (12-1-9)
        int pct = (int)(((idx + 1) * 100) / total_chunks);
        std::cout << "\r[파일 저장 중] " << pct << "% ("
                  << idx+1 << "/" << total_chunks << ")   " << std::flush;
    }

    if (success)
        std::cout << "\n[파일 저장 완료] " << resolved << "\n";  // 12-1-7

    g_file_transfer_in_progress = false;
}

// ─────────────────────────────────────────────────────────────────
//  handle_file_list  (0x0024)
// ─────────────────────────────────────────────────────────────────
void handle_file_list(int sock)
{
    init_dirs();

    std::cout << "폴더 필터 (없으면 Enter): ";
    std::string folder;
    std::getline(std::cin, folder);

    json req = make_request(PKT_FILE_LIST_REQ);
    req["user_no"]            = g_user_no;
    req["payload"]["folder"]  = folder;

    if (!send_json(sock, req)) {
        std::cout << "[오류] 목록 요청 전송 실패\n";
        return;
    }

    json resp;
    if (!recv_json(sock, resp)) {
        std::cout << "[오류] 서버 응답 수신 실패\n";
        return;
    }

    if (resp.value("code", -1) != VALUE_SUCCESS) {
        std::cout << "[오류] " << resp.value("msg", "목록 조회 실패") << "\n";
        return;
    }

    json& pl      = resp["payload"];
    json  files   = pl.value("files", json::array());
    int64_t used  = pl.value("storage_used",  (int64_t)0);
    int64_t total = pl.value("storage_total", (int64_t)0);

    system("clear");
    std::cout << "==========================================\n";
    std::cout << "  클라우드 파일 목록\n";
    std::cout << "  사용: " << human_size(used)
              << " / 전체: " << human_size(total) << "\n";
    std::cout << "------------------------------------------\n";

    if (files.empty()) {
        std::cout << "  (파일 없음)\n";
    } else {
        int i = 1;
        for (auto& f : files) {
            std::cout << "  [" << i++ << "] "
                      << f.value("file_name", "") << "  "
                      << human_size(f.value("file_size", (int64_t)0)) << "  "
                      << f.value("created_at", "");
            std::string fol = f.value("folder", "");
            if (!fol.empty()) std::cout << "  /" << fol;
            std::cout << "  (id=" << f.value("file_id", (int64_t)0) << ")\n";
        }
    }
    std::cout << "==========================================\n";
    std::cout << "계속하려면 Enter...";
    std::cin.get();
}

// ─────────────────────────────────────────────────────────────────
//  handle_file_upload  (0x0020 + 0x0021, 멀티스레드)
// ─────────────────────────────────────────────────────────────────
void handle_file_upload(int sock)
{
    init_dirs();

    // 이미 전송 중이면 거부 (12-1-6)
    if (g_file_transfer_in_progress.load()) {
        std::cout << "[파일 저장 중] 전송이 완료된 후에 다시 시도하세요.\n";
        std::cout << "계속하려면 Enter...";
        std::cin.get();
        return;
    }

    system("clear");
    std::cout << "==========================================\n";
    std::cout << "  파일 업로드\n";
    std::cout << "  기본 폴더: " << g_upload_dir << "\n";
    std::cout << "------------------------------------------\n";

    // 기본 폴더 파일 목록 출력 (12-1-2)
    try {
        int n = 1;
        for (auto& e : fs::directory_iterator(g_upload_dir)) {
            if (e.is_regular_file())
                std::cout << "  [" << n++ << "] "
                          << e.path().filename().string()
                          << "  " << human_size((int64_t)e.file_size()) << "\n";
        }
    } catch (...) {
        std::cout << "  (기본 폴더를 읽을 수 없음)\n";
    }

    std::cout << "------------------------------------------\n";
    std::cout << "파일명 또는 절대경로 입력: ";
    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) return;

    // 경로 결정: 절대경로이면 그대로, 아니면 기본 폴더 기준 (12-1-3)
    fs::path fpath = (input[0] == '/') ? fs::path(input)
                                       : fs::path(g_upload_dir) / input;

    // 파일 존재 확인 (12-1-4)
    if (!fs::exists(fpath) || !fs::is_regular_file(fpath)) {
        std::cout << "[오류] 그런 파일 없습니다: " << fpath.string() << "\n";
        std::cout << "계속하려면 Enter...";
        std::cin.get();
        return;
    }

    int64_t fsize = (int64_t)fs::file_size(fpath);
    std::cout << "\n파일명: " << fpath.filename().string()
              << "  크기: " << human_size(fsize) << "\n";
    std::cout << "서버에 저장하시겠습니까? (y/n): ";

    char yn;
    std::cin >> yn;
    std::cin.ignore();
    if (yn != 'y' && yn != 'Y') { std::cout << "취소\n"; return; }

    std::cout << "저장할 클라우드 폴더 (없으면 Enter=루트): ";
    std::string folder;
    std::getline(std::cin, folder);

    // 멀티스레드 업로드 시작 (12-1-5)
    g_file_transfer_in_progress = true;
    std::thread t(upload_thread, sock,
                  fpath.string(),
                  fpath.filename().string(),
                  folder);
    t.detach();

    std::cout << "[파일 저장 중] 백그라운드 전송 시작 - 다른 메뉴 이용 가능합니다.\n";
}

// ─────────────────────────────────────────────────────────────────
//  handle_file_download  (0x0022)
//  12-2-2: 수신 중 메시지 출력, 완료 메시지 출력
//  12-2-3: 서버 파일 삭제 안 함
// ─────────────────────────────────────────────────────────────────
void handle_file_download(int sock)
{
    init_dirs();

    // 이미 전송 중이면 거부 (12-1-6)
    if (g_file_transfer_in_progress.load()) {
        std::cout << "[파일 수신 중] 전송이 완료된 후에 다시 시도하세요.\n";
        std::cout << "계속하려면 Enter...";
        std::cin.get();
        return;
    }

    // 목록 먼저 출력
    handle_file_list(sock);

    std::cout << "다운로드할 file_id 입력: ";
    int64_t file_id = 0;
    std::cin >> file_id;
    std::cin.ignore();
    if (file_id <= 0) return;

    g_file_transfer_in_progress = true;

    // ── 0x0022 다운로드 요청 ─────────────────────────────────────
    json req = make_request(PKT_FILE_DOWNLOAD_REQ);
    req["user_no"]            = g_user_no;
    req["payload"]["file_id"] = file_id;

    if (!send_json(sock, req)) {
        std::cout << "[오류] 다운로드 요청 전송 실패\n";
        g_file_transfer_in_progress = false;
        return;
    }

    // META 응답 수신
    json resp;
    if (!recv_json(sock, resp)) {
        std::cout << "[오류] 서버 응답 수신 실패\n";
        g_file_transfer_in_progress = false;
        return;
    }

    if (resp.value("code", -1) != VALUE_SUCCESS) {
        std::cout << "[오류] " << resp.value("msg", "다운로드 실패") << "\n";
        g_file_transfer_in_progress = false;
        return;
    }

    json& mp          = resp["payload"];
    std::string fname = mp.value("file_name",    "file");
    int64_t fsize     = mp.value("file_size",    (int64_t)0);
    int64_t total     = mp.value("total_chunks", (int64_t)1);

    // 다운로드 저장 경로 (13-3-3)
    std::string save_path = g_download_dir + "/" + fname;
    std::cout << "\n[파일 수신 중] " << fname
              << " (" << human_size(fsize) << ")\n";
    std::cout << "저장 위치: " << save_path << "\n";

    // ── 청크 수신 루프 ────────────────────────────────────────────
    std::ofstream ofs(save_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        std::cout << "[오류] 파일 생성 실패: " << save_path << "\n";
        g_file_transfer_in_progress = false;
        return;
    }

    bool success = true;
    for (int64_t idx = 0; idx < total; ++idx) {
        json chunk;
        if (!recv_json(sock, chunk)) {
            std::cout << "\n[오류] 청크 수신 실패\n";
            success = false;
            break;
        }

        // 마지막으로 DONE 패킷이 먼저 올 경우 처리
        int type = chunk.value("type", 0);
        if (type == PKT_FILE_DOWNLOAD_REQ && chunk.value("msg","") == "다운로드 완료") break;

        json& cp    = chunk["payload"];
        std::string b64 = cp.value("data_b64", "");
        auto data   = b64_decode(b64);
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));

        int pct = (int)(((idx + 1) * 100) / total);
        std::cout << "\r[파일 수신 중] " << pct << "% ("
                  << idx+1 << "/" << total << ")   " << std::flush;
    }
    ofs.close();

    if (success) {
        // DONE 응답 수신 (서버가 마지막으로 전송)
        json done;
        recv_json(sock, done);
        std::cout << "\n[파일 수신 완료] " << fname << "\n"; // 12-2-2
    } else {
        fs::remove(save_path);
        std::cout << "[파일 수신 실패] 불완전 파일 삭제됨\n";
    }

    g_file_transfer_in_progress = false;
    std::cout << "계속하려면 Enter...";
    std::cin.get();
}

// ─────────────────────────────────────────────────────────────────
//  handle_file_delete  (0x0023)
// ─────────────────────────────────────────────────────────────────
void handle_file_delete(int sock)
{
    // 목록 먼저 출력
    handle_file_list(sock);

    std::cout << "삭제할 file_id 입력: ";
    int64_t file_id = 0;
    std::cin >> file_id;
    std::cin.ignore();
    if (file_id <= 0) return;

    std::cout << "정말 삭제하시겠습니까? (y/n): ";
    char yn;
    std::cin >> yn;
    std::cin.ignore();
    if (yn != 'y' && yn != 'Y') { std::cout << "취소\n"; return; }

    json req = make_request(PKT_FILE_DELETE_REQ);
    req["user_no"]            = g_user_no;
    req["payload"]["file_id"] = file_id;

    if (!send_json(sock, req)) {
        std::cout << "[오류] 삭제 요청 전송 실패\n";
        return;
    }

    json resp;
    if (!recv_json(sock, resp)) {
        std::cout << "[오류] 서버 응답 수신 실패\n";
        return;
    }

    if (resp.value("code", -1) == VALUE_SUCCESS)
        std::cout << "[파일 삭제 완료]\n";
    else
        std::cout << "[오류] " << resp.value("msg", "삭제 실패") << "\n";

    std::cout << "계속하려면 Enter...";
    std::cin.get();
}
