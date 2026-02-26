#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp>

using json = nlohmann::json;

// 메시지 전송
std::string handle_msg_send(const json& req, sql::Connection& db);

// 메시지 목록 조회
std::string handle_msg_list(const json& req, sql::Connection& db);

// 메시지 삭제
std::string handle_msg_delete(const json& req, sql::Connection& db);

// 메시지 읽음 처리
std::string handle_msg_read(const json& req, sql::Connection& db);