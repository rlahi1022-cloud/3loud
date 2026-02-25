#pragma once
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// ============================================================
// 공통 패킷 구조
// ============================================================

// 요청 패킷 생성
inline json make_req(int type, const json& payload = json::object())
{
    json pkt;
    pkt["type"] = type;
    pkt["payload"] = payload;
    return pkt;
}

// 응답 패킷 생성
inline json make_resp(int type, int code, const std::string& msg,
                      const json& payload = json::object())
{
    json pkt;
    pkt["type"] = type;
    pkt["code"] = code;
    pkt["msg"] = msg;
    pkt["payload"] = payload;
    return pkt;
}

// ============================================================
// Auth Schema
// ============================================================

namespace AuthSchema {

    // 로그인 payload
    inline json make_login_payload(const std::string& email,
                                   const std::string& pw_hash)
    {
        json pl;
        pl["email"] = email;
        pl["pw_hash"] = pw_hash;
        return pl;
    }

    // 회원가입 payload
    inline json make_signup_payload(const std::string& email,
                                    const std::string& pw_hash,
                                    const std::string& name)
    {
        json pl;
        pl["email"] = email;
        pl["pw_hash"] = pw_hash;
        pl["name"] = name;
        return pl;
    }

    // 로그인 요청 패킷
    inline json make_login_req(int type_login_req,
                               const std::string& email,
                               const std::string& pw_hash)
    {
        return make_req(type_login_req,
                        make_login_payload(email, pw_hash));
    }

    // 회원가입 요청 패킷
    inline json make_signup_req(int type_signup_req,
                                const std::string& email,
                                const std::string& pw_hash,
                                const std::string& name)
    {
        return make_req(type_signup_req,
                        make_signup_payload(email, pw_hash, name));
    }
}

// ============================================================
// Message Schema
// ============================================================

namespace MessageSchema {

    // 메시지 전송 payload
    inline json make_send_payload(const std::string& to,
                                  const std::string& content)
    {
        json pl;
        pl["to"] = to;             // 받는 사람
        pl["content"] = content;   // 메시지 내용
        return pl;
    }

    // 메시지 전송 요청 패킷
    inline json make_send_req(int type_msg_send_req,
                              const std::string& to,
                              const std::string& content)
    {
        return make_req(type_msg_send_req,
                        make_send_payload(to, content));
    }

    // 메시지 목록 조회 payload (필요시 확장)
    inline json make_list_payload()
    {
        return json::object();     // 현재는 빈 payload
    }

    inline json make_list_req(int type_msg_list_req)
    {
        return make_req(type_msg_list_req,
                        make_list_payload());
    }

    // 메시지 읽기 payload
    inline json make_read_payload(int msg_id)
    {
        json pl;
        pl["msg_id"] = msg_id;
        return pl;
    }

    inline json make_read_req(int type_msg_read_req,
                              int msg_id)
    {
        return make_req(type_msg_read_req,
                        make_read_payload(msg_id));
    }

    // 메시지 삭제 payload
    inline json make_delete_payload(int msg_id)
    {
        json pl;
        pl["msg_id"] = msg_id;
        return pl;
    }

    inline json make_delete_req(int type_msg_delete_req,
                                int msg_id)
    {
        return make_req(type_msg_delete_req,
                        make_delete_payload(msg_id));
    }
}