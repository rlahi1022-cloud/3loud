// ============================================================================
// 파일명: file_handler.hpp
// 목적: 서버 파일 기능 핸들러 선언
//
// [기존 코드와의 연결]
//   skeleton_server.cpp의 worker_loop switch(type)에 아래 case 추가:
//
//     case PKT_FILE_UPLOAD_REQ:
//         out_payload = handle_file_upload_req(req, *conn);
//         break;
//     case PKT_FILE_CHUNK:
//         out_payload = handle_file_chunk(req, *conn);
//         break;
//     case PKT_FILE_DOWNLOAD_REQ:
//         out_payload = handle_file_download_req(task.sock, req, *conn);
//         break;
//     case PKT_FILE_DELETE_REQ:
//         out_payload = handle_file_delete_req(req, *conn);
//         break;
//     case PKT_FILE_LIST_REQ:
//         out_payload = handle_file_list_req(req, *conn);
//         break;
//
//   main()에서 서버 시작 전:
//     file_handler_init("/srv/3loud/files");
//
// [패킷 구조] 기존 make_resp / json 구조 그대로 사용
//   요청: { "type": PKT_*, "payload": { ... } }
//   응답: { "type": PKT_*, "code": VALUE_*, "msg": "...", "payload": { ... } }
// ============================================================================
#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp>

using json = nlohmann::json;

// 서버 파일 저장 루트 경로 초기화 (main에서 1회 호출)
void file_handler_init(const std::string& cloud_root);

// 0x0020  업로드 요청 - 메타 검사 후 READY 응답
// req payload: { "file_name": str, "file_size": int64, "folder": str }
std::string handle_file_upload_req(const json& req, sql::Connection& db);

// 0x0021  청크 수신 - 파일 데이터 append, 마지막 청크면 DB INSERT
// req payload: { "file_name": str, "folder": str,
//                "chunk_index": int, "total_chunks": int,
//                "data_b64": str, "file_size": int64 }
std::string handle_file_chunk(const json& req, sql::Connection& db);

// 0x0022  다운로드 요청 - sock에 청크를 직접 전송 후 DONE 응답 반환
// req payload: { "file_id": int64 }
std::string handle_file_download_req(int sock, const json& req, sql::Connection& db);

// 0x0023  파일 삭제 - 파일시스템 + DB 삭제
// req payload: { "file_id": int64 }
std::string handle_file_delete_req(const json& req, sql::Connection& db);

// 0x0024  파일 목록 - DB SELECT 후 JSON 배열 반환
// req payload: { "folder": str }  (빈 문자열이면 전체)
std::string handle_file_list_req(const json& req, sql::Connection& db);
