#ifndef JSON_PACKET_HPP
#define JSON_PACKET_HPP

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// ===============================
// 공통 요청 템플릿 생성
// ===============================
inline json make_request(int type)
{
    json j;
    j["type"] = type;
    j["payload"] = json::object();
    return j;
}

// ===============================
// 공통 응답 템플릿 생성
// ===============================
inline json make_response(int type, int code)
{
    json j;
    j["type"] = type;
    j["code"] = code;
    j["msg"] = "";
    j["payload"] = json::object();
    return j;
}

// ===============================
// 메세지 템플릿 생성
// ===============================

inline json make_optimized_response(int type, int code, const json& value = json::object()) {
    json j;
    j["type"] = type;
    j["payload"] = {
        {"code", code},      // [지침 준수] code를 payload 내부로 이동
        {"value", value}     // [지침 준수] 데이터를 value 필드에 배치
    };
    return j;
}

// ===============================
// 안전한 payload 접근
// ===============================
inline json get_payload(const json& j)
{
    return j.value("payload", json::object());
}

#endif