#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp>

using json = nlohmann::json;

// 읽지 않은 메시지 폴링 (세션 없이 email+pw_hash 인증)
std::string handle_msg_poll(const json& req, sql::Connection& db);

// 메시지 전송
std::string handle_msg_send(const json& req, sql::Connection& db);

// 메시지 목록 조회
std::string handle_msg_list(const json& req, sql::Connection& db);

// 메시지 삭제
std::string handle_msg_delete(const json& req, sql::Connection& db);

// 메시지 읽음 처리
std::string handle_msg_read(const json& req, sql::Connection& db);

// 메시지 설정 조회
std::string handle_msg_setting_get(const nlohmann::json& req, sql::Connection& db);

// 메시지 설정 저장
std::string handle_msg_setting_save(const nlohmann::json& req, sql::Connection& db);