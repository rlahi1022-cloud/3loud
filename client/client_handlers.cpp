// ============================================================================
// 파일명: client/client_handlers.cpp
// ============================================================================

#include "client_handlers.h"
#include "client_net.hpp"
#include "protocol.h"
#include "protocol_schema.h"
#include "json_packet.hpp"
#include "sha256.h"
#include "file_client.hpp"
#include <iostream>
#include <string>
#include <limits>
#include <cctype>
#include <algorithm>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/select.h> // select()
#include <sys/time.h>   // timeval
#include <unistd.h>     // usleep
#include <iomanip>      // setfill, setw (시간 포맷팅용)

using namespace std;

// ============================================================================
// [내부 유틸] 입력 버퍼 비우기 및 문자열 입력 받기
// ============================================================================
static void clear_input()
{
    cin.clear();
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
}

static string get_input(const string &prompt)
{
    string input;
    cout << prompt;
    if (!(cin >> input))
    {
        clear_input();
        return "";
    }
    return input;
}

static void wait_for_enter()
{
    // 1. 이전에 cin >> 등으로 입력받고 남은 \n(엔터) 찌꺼기를 제거
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    // 2. 안내 메시지 출력
    cout << "\n[엔터를 눌러 진행해주세요...]";

    // 3. 실제 사용자의 엔터 키 입력 대기
    cin.get();
}

// [내부 유틸] 비밀번호 입력 받기 (화면에 *로 표시) - ★ 새로 추가된 함수
// ============================================================================
static string get_password_input(const string &prompt)
{
    string pw;
    struct termios oldt, newt;

    // 1. 현재 터미널 설정 백업
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;

    // 2. ECHO(화면 출력)와 ICANON(엔터 칠 때까지 버퍼링) 끄기
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    cout << prompt;
    fflush(stdout); // 버퍼 강제 비움 (설정 변경 후 즉시 출력 위해)

    char ch;
    while (true)
    {
        ch = getchar(); // 키보드 입력 1바이트 읽기

        if (ch == '\n' || ch == '\r') // 엔터키
        {
            cout << endl;
            break;
        }
        else if (ch == 127 || ch == 8) // 백스페이스 (ASCII 127 or 8)
        {
            if (!pw.empty())
            {
                pw.pop_back();   // 문자열에서 제거
                cout << "\b \b"; // 화면에서 별표 지우기 (뒤로-공백-뒤로)
                fflush(stdout);
            }
        }
        else
        {
            pw += ch;
            cout << "*"; // 화면엔 별표 출력
            fflush(stdout);
        }
    }

    // 3. 터미널 설정 복구
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    return pw;
}

//============================================
// [유틸] 타이머와 함께 입력 받기 (Linux용, 인증번호 카운트다운)
static string get_input_with_timer(const string &prompt, int limit_seconds)
{
    string input;
    struct termios oldt, newt;
    tcflush(STDIN_FILENO, TCIFLUSH); // 리눅스버퍼다비우기
    time_t start_time = time(NULL);

    // 1. 터미널 설정 변경 (Canonical Mode Off, Echo Off) -> 키 입력 즉시 감지
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    cout << prompt; // 초기 프롬프트 출력

    while (true)
    {
        // 2. 남은 시간 계산
        time_t now = time(NULL);
        int elapsed = (int)(now - start_time);
        int remain = limit_seconds - elapsed;

        if (remain <= 0)
        {
            input = ""; // 시간 초과 시 입력 무효화
            break;
        }

        // 3. 커서를 줄 맨 앞으로 옮기고(\r) 시간과 입력된 내용 덮어쓰기
        // \033[K 는 커서 뒤쪽의 잔여 문자를 지우는 ANSI 코드입니다.
        cout << "\r" << prompt
             << " [남은 시간: "
             << setfill('0') << setw(2) << (remain / 60) << ":"
             << setfill('0') << setw(2) << (remain % 60) << "] : "
             << input << " \033[K" << flush;

        // 4. 키보드 입력 감지 (select 사용)
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 0.1초 대기 (CPU 점유율 방지)

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

        if (ret > 0) // 키 입력이 있음!
        {
            char ch = getchar();

            if (ch == '\n' || ch == '\r') // 엔터키
            {
                cout << endl;
                break;
            }
            else if (ch == 127 || ch == 8) // 백스페이스
            {
                if (!input.empty())
                {
                    input.pop_back();
                }
            }
            else if (isprint(ch)) // 일반 출력 가능 문자
            {
                input += ch;
            }
        }
        // ret == 0 이면 입력 없음 (루프 돌면서 시간만 갱신)
    }

    // 5. 터미널 설정 원상 복구
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    return input;
}

// ============================================================================
// [검증 함수 1] 이메일 형식 검사
// 조건: '@' 문자가 포함되어 있어야 함 (간단한 체크)
// ============================================================================
static bool is_valid_email(const string &email)
{
    if (email.length() < 5)
        return false; // 최소 a@b.c 길이

    // '@' 가 없거나, 맨 앞/맨 뒤에 있으면 탈락
    size_t at_pos = email.find('@');
    if (at_pos == string::npos || at_pos == 0 || at_pos == email.length() - 1)
    {
        return false;
    }

    return true;
}

// ============================================================================
// [검증 함수 2] 비밀번호 복잡도 검사
// 조건: 10자리 이상 + 영문/숫자/특수문자 모두 포함
// ============================================================================
static bool is_valid_pw(const string &pw)
{
    // 1. 길이 체크
    if (pw.length() < 10)
        return false;

    bool has_alpha = false;
    bool has_digit = false;
    bool has_spec = false;

    for (char c : pw)
    {
        if (isalpha(c))
            has_alpha = true; // 영문
        else if (isdigit(c))
            has_digit = true; // 숫자
        else if (ispunct(c))
            has_spec = true; // 특수문자
    }

    // 세 가지 조건 모두 만족해야 함
    return has_alpha && has_digit && has_spec;
}

// ============================================================================
// [핸들러 1] 로그인
// ============================================================================
bool handle_login(int sock)
{
    cout << "\n[로그인 (뒤로 가기 : /c)]\n";

    // 1. 입력 받기
    string email = get_input("이메일: ");
    getchar(); // 개행문자 비우기용
    string pw = get_password_input("비밀번호: ");

    // 2. 필수 값 체크 (수정: 바로 리턴하지 않고 메시지 출력 + 대기)
    if (email == "/c" || pw == "/c")
    {
        cout << "로비로 돌아갑니다.\n";
        wait_for_enter();
        return false;
    }

    if (email.empty() || pw.empty())
    {
        cout << ">> [오류] 이메일과 비밀번호를 모두 입력해주세요." << endl;
        wait_for_enter();
        return false;
    }

    // 3. 비밀번호 해싱 (서버와 동일한 알고리즘 필수)
    string hashed_pw = sha256(pw);
    g_current_pw_hash = hashed_pw;  // 폴링 소켓 재로그인용

    // 4.스키마를 이용해 요청 패킷 생성
    json req = AuthSchema::make_login_req(PKT_AUTH_LOGIN_REQ, email, hashed_pw);

    // 5. 서버 전송 (수정: 실패 시 메시지 + 대기)
    if (!send_json(sock, req))
    {
        cout << ">> [오류] 서버에 요청을 보낼 수 없습니다." << endl;
        wait_for_enter();
        return false;
    }

    // 6. 응답 수신 (수정: 실패 시 메시지 + 대기)
    json res;
    if (!recv_json(sock, res))
    {
        cout << ">> [오류] 서버로부터 응답을 받지 못했습니다." << endl;
        wait_for_enter();
        return false;
    }

    // 7. 결과 처리
    int code = res.value("code", -1);
    string msg = res.value("msg", "알 수 없는 오류");

    if (code == VALUE_SUCCESS)
    {
        // [수정] 서버가 보낸 payload에서 user_no를 꺼내 전역 변수에 저장
        if (res.contains("payload"))
        {
            json payload = res["payload"];
            // 서버가 보낸 "user_no" 값을 읽어서 g_user_no에 저장
            // (만약 user_no가 없으면 0으로 설정)
            g_user_no = payload.value("user_no", (uint32_t)0);
        }
        g_current_user_email = email;  // 폴링용 이메일 저장

        cout << ">> [로그인 성공] " << msg << endl;
        wait_for_enter();
        return true;
    }
    else
    {
        // 서버에서 보낸 에러 메시지 출력 (예: 비밀번호 불일치 등)
        cout << ">> [로그인 실패] " << msg << endl;
        wait_for_enter(); // 요청하신 기능: 엔터 대기
        return false;
    }
}

// ============================================================================
// [핸들러 2] 회원가입 (검증 -> 요청 -> 인증)
// ============================================================================
void handle_signup(int sock)
{
    cout << "\n[회원가입 (뒤로 가기 : /c)]\n";

    string email, pw, nickname;
    // --- 1. 이메일 입력 및 검증 ---
    while (true)
    {
        email = get_input("이메일 (ID): ");

        if (email == "/c")
        {
            cout << "로비로 돌아갑니다.\n";
            wait_for_enter();
            return;
        }

        if (email.empty())
            return; // 취소

        if (is_valid_email(email))
        {
            break; // 통과
        }
        else
        {
            cout << ">> [경고] 올바른 이메일 형식이 아닙니다. (예: user@example.com)\n";
        }
    }
    clear_input();
    // --- 2. 비밀번호 입력 및 검증  ---
    while (true)
    {
        // 2-1. 첫 번째 비밀번호 입력
        pw = get_password_input("비밀번호 (영문+숫자+특수문자, 10자 이상): ");
        if (pw == "/c")
        {
            cout << "로비로 돌아갑니다.\n";
            wait_for_enter();
            return;
        }

        if (pw.empty())
            return; // 취소

        // 2-2. 비밀번호 규칙 검증
        if (!is_valid_pw(pw))
        {
            cout << ">> [경고] 비밀번호는 10자 이상, 영문/숫자/특수문자를 모두 포함해야 합니다.\n";
            continue; // 규칙에 맞지 않으면 재입력
        }

        // 2-3. 비밀번호 재확인 입력
        string pw_check = get_password_input("비밀번호 재확인: ");

        if (pw_check == "/c")
        {
            cout << "로비로 돌아갑니다.\n";
            wait_for_enter();
            return;
        }

        if (pw_check.empty())
            return; // 취소

        // 2-4. 일치 여부 확인
        if (pw == pw_check)
        {
            break; // 두 비밀번호가 같으면 루프 탈출 (성공)
        }
        else
        {
            cout << ">> [경고] 비밀번호가 일치하지 않습니다. 다시 입력해주세요.\n";
            // 루프가 다시 돌면서 비밀번호를 처음부터 다시 입력받음
        }
    }

    // --- 3. 닉네임 입력 ---
    nickname = get_input("닉네임: ");

    if (nickname == "/c")
    {
        cout << "로비로 돌아갑니다.\n";
        wait_for_enter();
        return;
    }

    if (nickname.empty())
        return;

    // --- 4. 서버로 인증번호 발송 요청 ---
    cout << ">> 서버에 인증번호를 요청하고 있습니다...\n";

    string hashed_pw = sha256(pw); // 해싱

    // 서버로 통신
    json req = AuthSchema::make_signup_req(PKT_AUTH_REGISTER_REQ, email, hashed_pw, nickname);

    if (!send_json(sock, req))
    {
        cout << ">> 전송 실패\n";
        return;
    }

    json res;
    if (!recv_json(sock, res))
    {
        cout << ">> 수신 실패\n";
        return;
    }

    // 서버가 "이미 가입된 이메일입니다" 등을 보냈을 경우 처리
    if (res["code"] != VALUE_SUCCESS)
    {
        cout << ">> [가입 요청 반려] " << res.value("msg", "Unknown Error") << endl;
        wait_for_enter();
        return;
    }

    // --- 5. 인증번호 입력 단계 ---
    cout << ">> [인증번호 발송 완료] " << email << "으로 전송된 코드를 입력하세요.\n";

    cin.ignore(numeric_limits<streamsize>::max(), '\n'); // 버퍼 비우기

    // [수정] 3회 시도 로직 추가
    int max_attempts = 3;           // 최대 시도 횟수
    int total_limit_seconds = 90;   // 전체 제한 시간 (1분 30초)
    time_t start_time = time(NULL); // 타이머 시작 시간

    for (int i = 0; i < max_attempts; ++i)
    {
        // 1. 남은 시간 계산
        time_t now = time(NULL);
        int elapsed = (int)(now - start_time);         // 흐른 시간
        int remaining = total_limit_seconds - elapsed; // 남은 시간

        // 시간이 이미 다 지났다면 루프 종료
        if (remaining <= 0)
        {
            cout << "\n>> [시간 초과] 전체 인증 시간이 만료되었습니다.\n";
            wait_for_enter();
            return;
        }

        // 2. 남은 시간을 인자로 주어 입력 함수 호출
        // (i가 0보다 크면 재시도라는 뜻이므로 안내 메시지 변경 가능)
        string prompt = (i == 0) ? "인증번호 입력: " : "인증번호 재입력: ";
        string code_input = get_input_with_timer(prompt, remaining);

        if (code_input == "/c")
        {
            cout << "로비로 돌아갑니다.\n";
            wait_for_enter();
            return;
        }

        // 시간 초과 등으로 입력이 없으면 중단 (get_input_with_timer 내부에서 타임아웃 처리됨)
        if (code_input.empty())
        {
            cout << "\n>> [시간 초과] 입력 시간이 만료되었습니다.\n";
            wait_for_enter();
            return;
        }

        // 3. 서버로 검증 요청 전송
        json verify_req = make_request(PKT_AUTH_VERIFY_REQ);
        verify_req["payload"] = {
            {"email", email},
            {"code", code_input}};

        if (!send_json(sock, verify_req))
        {
            cout << ">> [전송 오류] 서버와 연결이 끊어졌습니다.\n";
            return;
        }

        if (!recv_json(sock, res))
        {
            cout << ">> [수신 오류] 응답을 받지 못했습니다.\n";
            return;
        }

        // 4. 결과 확인
        if (res["code"] == VALUE_SUCCESS)
        {
            cout << "\n>> [가입 성공] " << res.value("msg", "환영합니다!") << endl;
            wait_for_enter();
            return; // 성공했으니 함수 완전 종료
        }

        else if (res["code"] == VALUE_ERR_SESSION)
        {
            // [추가] 중요: 세션 만료 에러가 오면 재시도하지 말고 즉시 종료
            cout << "\n>> [실패] " << res.value("msg", "세션이 만료되었습니다.") << endl;
            wait_for_enter();
            return;
        }

        else
        {
            // 실패 시 메시지 출력 및 남은 횟수 안내
            cout << "\n>> [인증 실패] " << res.value("msg", "코드가 일치하지 않습니다.");
            if (i < max_attempts - 1)
            {
                cout << " (남은 기회: " << (max_attempts - 1 - i) << "번)" << endl;
                cout << ">> 다시 시도해주세요.\n";
            }
            else
            {
                cout << "\n>> [실패] 인증 시도 횟수를 모두 소진했습니다.\n";
            }
        }
    }

    // 3번 다 틀렸거나 실패하고 나온 경우
    wait_for_enter();
}

// ============================================================================
// [핸들러 3] 기타 기능 (스텁)
// ============================================================================
void handle_logout(int sock) { cout << ">> 로그아웃 완료\n"; }
// void handle_file_list(int sock) { cout << ">> [미구현] 파일 목록\n"; }
// void handle_file_upload(int sock) { cout << ">> [미구현] 파일 업로드\n"; }
// void handle_file_download(int sock) { cout << ">> [미구현] 파일 다운로드\n"; }
// void handle_message_menu(int sock) { cout << ">> [미구현] 메시지 메뉴\n"; }
void handle_profile_menu(int sock)
{
    while (true)
    {
        // 화면 지우기
        printf("\033[H\033[J");

        cout << "==========================================\n";
        cout << "  [개인 설정] (User No: " << g_user_no << ")\n";
        cout << "------------------------------------------\n";
        cout << "  1. 이메일 변경\n";
        cout << "  2. 비밀번호 변경\n";
        cout << "  3. 닉네임 변경\n";
        cout << "  0. 뒤로 가기\n";
        cout << "==========================================\n";
        cout << "선택 > ";

        int choice;
        if (!(cin >> choice))
        {
            clear_input();
            continue;
        }
        clear_input(); // 버퍼 비우기

        if (choice == 0)
            return;

        string update_type;
        string input_value;
        string prompt_msg;

        if (choice == 1)
        {
            update_type = "email";
            prompt_msg = "변경할 새 이메일: ";
        }
        else if (choice == 2)
        {
            update_type = "pw";
            prompt_msg = "변경할 새 비밀번호: ";
        }
        else if (choice == 3)
        {
            update_type = "nickname";
            prompt_msg = "변경할 새 닉네임: ";
        }
        else
        {
            cout << "잘못된 선택입니다.\n";
            wait_for_enter();
            continue;
        }

        cout << prompt_msg;
        getline(cin, input_value);

        if (input_value.empty())
        {
            cout << "값을 입력해주세요.\n";
            wait_for_enter();
            continue;
        }

        // 비밀번호는 해싱하여 전송
        if (update_type == "pw")
        {
            input_value = sha256(input_value);
        }

        // 패킷 생성
        json req = make_request(PKT_SETTINGS_SET_REQ);
        req["user_no"] = g_user_no;
        req["payload"]["update_type"] = update_type;
        req["payload"]["value"] = input_value;

        // 전송 및 수신
        if (!send_json(sock, req))
        {
            cout << "[오류] 서버 요청 실패\n";
            break;
        }

        json resp;
        if (!recv_json(sock, resp))
        {
            cout << "[오류] 서버 응답 수신 실패\n";
            break;
        }

        cout << "\n------------------------------------------\n";
        if (resp.value("code", -1) == VALUE_SUCCESS)
        {
            cout << ">> [성공] " << resp.value("msg", "변경 완료") << "\n";
        }
        else
        {
            cout << ">> [실패] " << resp.value("msg", "변경 실패") << "\n";
        }
        cout << "------------------------------------------\n";
        wait_for_enter();
    }
}