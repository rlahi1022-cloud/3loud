#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp> // [확인] MariaDB 커넥터 직접 참조

using json = nlohmann::json;


std::string handle_blacklist_process(const json& req, sql::Connection& db);
std::string handle_blacklist_add(const json& req, sql::Connection& db);
std::string handle_blacklist_remove(const json& req, sql::Connection& db);
std::string handle_blacklist_list(const json& req, sql::Connection& db);