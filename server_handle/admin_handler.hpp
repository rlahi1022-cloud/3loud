
#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp>

// 1. 유저 목록 조회 (전체 또는 비활성)
std::string handle_admin_user_list(const nlohmann::json &req, sql::Connection &db);

// 2. 유저 상세 정보 조회 (용량 조인)
std::string handle_admin_user_info(const nlohmann::json &req, sql::Connection &db);

// 3. 계정 상태 변경
std::string handle_admin_state_change(const nlohmann::json &req, sql::Connection &db);