#pragma once
// ============================================================================
// 파일명: settings_handler.hpp
// 목적: 클라이언트 파일설정 메뉴에서 서버에 요청하는 설정 처리 핸들러 선언
//
// 처리 패킷:
//   PKT_SETTINGS_GET_REQ (0x0030) - 설정 조회 (현재: 용량 정보)
//   PKT_SETTINGS_SET_REQ (0x0031) - 설정 변경 (폴더 생성 / 폴더 삭제)
//
// skeleton_server.cpp 의 switch(type) 에 아래 case 추가:
//   case PKT_SETTINGS_GET_REQ:
//       out_payload = handle_settings_get(req, *conn);
//       break;
//   case PKT_SETTINGS_SET_REQ:
//       out_payload = handle_settings_set(req, *conn);
//       break;
//
// 서버 파일 저장 루트는 file_handler.cpp 와 동일한 경로 공유
//   extern std::string g_cloud_root;  // file_handler.cpp 에서 정의
// ============================================================================

#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp>

using json = nlohmann::json;

// PKT_SETTINGS_GET_REQ (0x0030)
// payload: { "query": "storage" }
// 응답  : { "storage_used": int64, "storage_total": int64 }
std::string handle_settings_get(const json& req, sql::Connection& db);

// PKT_SETTINGS_SET_REQ (0x0031)
// payload action = "create_folder" → { "folder": str }
// payload action = "delete_folder" → { "folder": str }  (내부 파일 있으면 오류)
std::string handle_settings_set(const json& req, sql::Connection& db);
