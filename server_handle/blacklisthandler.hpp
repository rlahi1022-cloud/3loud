#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp>


std::string handle_server_blacklist_process(const nlohmann::json& req, sql::Connection& db);
std::string handle_server_blacklist_add(const nlohmann::json& req, sql::Connection& db);
std::string handle_server_blacklist_remove(const nlohmann::json& req, sql::Connection& db);
std::string handle_server_blacklist_list(const nlohmann::json& req, sql::Connection& db);