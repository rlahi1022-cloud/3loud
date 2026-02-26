#include "profile_handler.hpp"
#include "protocol.h"
#include "protocol_schema.h"
#include "json_packet.hpp"
#include <iostream>
#include <string>
#include <limits>
#include <cctype>
#include <algorithm>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <iomanip>
#include <mariadb/conncpp.hpp>
#include <mutex>
#include <map> // map 헤더 추가

// [중요 1] 다른 파일(skeleton_server.cpp)에 있는 전역 변수를 가져다 쓰기 위해 선언
extern std::mutex g_fail_m;
extern std::map<std::string, int> g_fail_counts;

// server_handle/setting_handler.cpp

// ... (헤더 포함)

extern std::mutex g_fail_m;
extern std::map<std::string, int> g_fail_counts;

std::string handle_settings_verify_req(const json &req, sql::Connection &db)
{
    // 1. 패킷 파싱
    int user_no = req.value("user_no", 0);
    json payload;
    try
    {
        payload = req.at("payload");
    }
    catch (...)
    {
        return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_ERR_INVALID_PACKET, "Payload Error", json::object()).dump();
    }

    std::string client_pw_hash = payload.value("pw_hash", "");
    if (user_no == 0 || client_pw_hash.empty())
        return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_ERR_INVALID_PACKET, "잘못된 요청", json::object()).dump();

    try
    {
        // 2. DB 조회
        std::unique_ptr<sql::PreparedStatement> st(db.prepareStatement("SELECT email, pw_hash, is_active FROM users WHERE no = ?"));
        st->setInt(1, user_no);
        std::unique_ptr<sql::ResultSet> res(st->executeQuery());

        if (res->next())
        {
            std::string email = res->getString("email").c_str();
            std::string db_pw_hash = res->getString("pw_hash").c_str();
            int is_active = res->getInt("is_active");

            // 2-1. 이미 정지된 계정인지 체크
            if (is_active == 0)
            {
                // 이미 정지된 상태라면 즉시 권한 없음 리턴
                return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_ERR_PERMISSION, "계정이 정지되었습니다.", json::object()).dump();
            }

            // 3. 비밀번호 비교
            if (db_pw_hash == client_pw_hash)
            {
                // [성공] 실패 카운트 초기화
                {
                    std::lock_guard<std::mutex> lock(g_fail_m);
                    g_fail_counts.erase(email);
                }
                return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_SUCCESS, "인증 성공", json::object()).dump();
            }
            else
            {
                // [실패] 카운트 증가
                int current_fail = 0;
                {
                    std::lock_guard<std::mutex> lock(g_fail_m);
                    current_fail = ++g_fail_counts[email];
                }

                // 3-1. ★ 5회 도달 시 정지 처리 (VALUE_ERR_PERMISSION)
                if (current_fail >= 5)
                {
                    std::unique_ptr<sql::PreparedStatement> lock_st(db.prepareStatement("UPDATE users SET is_active = 0 WHERE email = ?"));
                    lock_st->setString(1, email);
                    lock_st->executeUpdate();

                    {
                        std::lock_guard<std::mutex> lock(g_fail_m);
                        g_fail_counts.erase(email);
                    }
                    std::cout << ">> [계정 정지] " << email << " (설정 메뉴 비번 5회 오류)\n";

                    // ★ 중요: 5회 넘었을 때만 PERMISSION 에러 전송
                    return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_ERR_PERMISSION,
                                     "비밀번호 5회 오류로 계정이 정지되었습니다. 강제 로그아웃됩니다.", json::object())
                        .dump();
                }

                // 3-2. 단순 실패
                std::string msg = "비밀번호 불일치 (" + std::to_string(current_fail) + "/5)";
                return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_ERR_LOGIN_PW, msg, json::object()).dump();
            }
        }
        else
        {
            return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_ERR_UNKNOWN, "사용자 정보 없음", json::object()).dump();
        }
    }
    catch (sql::SQLException &e)
    {
        return make_resp(PKT_SETTINGS_VERIFY_REQ, VALUE_ERR_DB, "DB Error", json::object()).dump();
    }
}

// [중요 3] static 제거
std::string handle_settings_set_req(const json &req, sql::Connection &db)
{
    // 1. 패킷 파싱
    int user_no = req.value("user_no", 0);
    json payload;
    try
    {
        payload = req.at("payload"); // at()을 써서 예외 처리를 유도하거나 체크
    }
    catch (...)
    {
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET, "Payload Missing", json::object()).dump();
    }

    std::string type = payload.value("update_type", "");
    std::string value = payload.value("value", "");

    if (user_no == 0 || type.empty() || value.empty())
    {
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET, "잘못된 요청입니다.", json::object()).dump();
    }

    try
    {
        std::unique_ptr<sql::PreparedStatement> st;

        // 2. 타입에 따른 쿼리 준비
        if (type == "email")
        {
            st.reset(db.prepareStatement("UPDATE users SET email = ? WHERE no = ?"));
        }
        else if (type == "pw")
        {
            st.reset(db.prepareStatement("UPDATE users SET pw_hash = ? WHERE no = ?"));
        }
        else if (type == "nickname")
        {
            st.reset(db.prepareStatement("UPDATE users SET nickname = ? WHERE no = ?"));
        }
        else if (type == "grade")
        {
            st.reset(db.prepareStatement("UPDATE users SET grade = ? WHERE no = ?"));
        }
        else
        {
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_INVALID_PACKET, "알 수 없는 설정 타입", json::object()).dump();
        }
        // 값 바인딩 (grade는 int 컬럼이지만 setString으로 넣어도 MariaDB가 자동 형변환 처리함)
        st->setString(1, value);
        st->setInt(2, user_no);

        int rows = st->executeUpdate();
        if (rows > 0)
        {
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_SUCCESS, "변경되었습니다.", json::object()).dump();
        }
        else
        {
            return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_DB, "변경 실패 (DB 오류)", json::object()).dump();
        }
    }
    catch (sql::SQLException &e)
    {
        return make_resp(PKT_SETTINGS_SET_REQ, VALUE_ERR_DB, "DB 에러 발생", json::object()).dump();
    }
}