#pragma once
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// =====================
// 공통 패킷 구조
// =====================

inline json make_packet(int type, const json& payload)
{
    return {
        {"type", type},
        {"payload", payload}
    };
}

// =====================
// Auth Payload 구조
// =====================

namespace AuthSchema {

    inline json make_login_req(const std::string& id,
                               const std::string& pw)
    {
        return {
            {"id", id},
            {"pw", pw}
        };
    }

    inline json make_signup_req(const std::string& id,
                                const std::string& pw,
                                const std::string& nickname)
    {
        return {
            {"id", id},
            {"pw", pw},
            {"nickname", nickname}
        };
    }
}