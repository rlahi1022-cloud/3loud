#pragma once

#include "json_packet.hpp"
#include <string>
#include <nlohmann/json.hpp>
#include <mariadb/conncpp.hpp>

std::string handle_settings_verify_req(const json &req, sql::Connection &db);
std::string handle_settings_set_req(const json &req, sql::Connection &db);