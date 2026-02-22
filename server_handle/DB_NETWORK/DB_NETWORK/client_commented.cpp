// ============================================================================
// 파일명: client.cpp
// 설명: 인증 클라이언트 + FTP 파일 전송 클라이언트
// 기능: 회원가입, 로그인, 파일 업로드/다운로드, 파일 목록 조회
// ============================================================================

// ============================================================================
// 헤더 파일 포함 (Include Headers)
// ============================================================================
#include <iostream>           // 표준 입출력 (cout, cin 등)
#include <string>             // 문자열 클래스 (std::string)
#include <cstring>            // C 스타일 문자열 함수
#include <sys/socket.h>       // 소켓 프로그래밍 (socket, connect 등)
#include <arpa/inet.h>        // IP 주소 변환 (inet_pton)
#include <unistd.h>           // 유닉스 표준 함수 (close, read, write)
#include <nlohmann/json.hpp>  // JSON 라이브러리 (데이터 파싱/생성)
#include <termios.h>          // 터미널 I/O 설정 (비밀번호 마스킹)
#include <fstream>            // 파일 입출력 (파일 업로드/다운로드)

using json = nlohmann::json;  // json 타입을 짧게 사용하기 위한 별칭

// ============================================================================
// 비밀번호 유효성 검증 함수 (클라이언트용)
// 설명: 서버로 전송하기 전에 클라이언트에서도 미리 검증
// 규칙: 8자 이상, 대소문자/숫자/특수문자 각 1개 이상 포함
// 매개변수: password - 검증할 비밀번호
// 반환값: true (유효) / false (무효)
// ============================================================================
bool isValidPassword(const std::string& password) {
    // 최소 길이 체크
    if (password.length() < 8) {
        return false;                          // 8자 미만이면 무효
    }

    // 각 문자 타입의 포함 여부를 추적하는 플래그
    bool hasUpper = false;                     // 대문자 포함 여부
    bool hasLower = false;                     // 소문자 포함 여부
    bool hasDigit = false;                     // 숫자 포함 여부
    bool hasSpecial = false;                   // 특수문자 포함 여부
    std::string specialChars = "!@#$%^&*()_+-=[]{}|;:,.<>?";  // 허용되는 특수문자 목록

    // 비밀번호의 각 문자를 검사
    for (char c : password) {
        if (isupper(c)) hasUpper = true;       // 대문자 발견
        else if (islower(c)) hasLower = true;  // 소문자 발견
        else if (isdigit(c)) hasDigit = true;  // 숫자 발견
        else if (specialChars.find(c) != std::string::npos) hasSpecial = true;  // 특수문자 발견
    }

    // 모든 조건을 만족해야 true 반환
    return hasUpper && hasLower && hasDigit && hasSpecial;
}

// ============================================================================
// 이메일 유효성 검증 함수 (클라이언트용)
// 설명: 이메일 주소가 올바른 형식인지 간단히 확인
// 규칙: 최소 1개의 @와 . 포함, @ 다음에 .이 와야 함
// 매개변수: email - 검증할 이메일 주소
// 반환값: true (유효) / false (무효)
// ============================================================================
bool isValidEmail(const std::string& email) {
    // @ 위치 찾기
    size_t at_pos = email.find('@');
    // 마지막 . 위치 찾기
    size_t dot_pos = email.rfind('.');
    
    // 조건:
    // 1. @ 존재
    // 2. . 존재
    // 3. @ 다음에 .이 옴
    // 4. @가 처음이 아님
    // 5. .이 마지막이 아님
    return (at_pos != std::string::npos &&        // @ 존재
            dot_pos != std::string::npos &&       // . 존재
            at_pos < dot_pos &&                   // @ 다음에 .
            at_pos > 0 &&                         // @가 처음 아님
            dot_pos < email.length() - 1);        // .이 마지막 아님
}

// ============================================================================
// 전화번호 유효성 검증 함수 (클라이언트용)
// 설명: 한국 전화번호 형식인지 확인
// 형식: 010-1234-5678 또는 011-123-4567 (총 13자)
// 매개변수: phone - 검증할 전화번호
// 반환값: true (유효) / false (무효)
// ============================================================================
bool isValidPhoneNumber(const std::string& phone) {
    // 길이 체크 (13자: 01X-XXXX-XXXX)
    if (phone.length() != 13) return false;
    
    // 01로 시작하는지 체크
    if (phone[0] != '0' || phone[1] != '1') return false;
    
    // - 위치 체크 (3번째, 8번째 문자)
    if (phone[3] != '-' || phone[8] != '-') return false;
    
    // 각 자리가 숫자인지 체크
    for (size_t i = 0; i < phone.length(); i++) {
        if (i == 3 || i == 8) continue;        // - 위치는 건너뜀
        if (!isdigit(phone[i])) return false;  // 숫자가 아니면 무효
    }
    
    return true;                               // 모든 검증 통과
}

// ============================================================================
// 비밀번호 입력 함수 (실시간 마스킹)
// 설명: 비밀번호를 입력받으면서 *로 표시
// 기능: 
//   - 각 문자 입력 시 즉시 * 출력
//   - Backspace 지원 (* 삭제)
//   - Enter로 입력 완료
// 반환값: 입력된 비밀번호 문자열
// ============================================================================
std::string getPasswordInput() {
    std::string password;                      // 실제 비밀번호 저장
    
    // 터미널 설정 변경 (에코 끄기, 버퍼링 끄기)
    termios oldt, newt;                        // 터미널 설정 구조체
    tcgetattr(STDIN_FILENO, &oldt);            // 현재 설정 저장
    newt = oldt;                               // 복사
    newt.c_lflag &= ~(ICANON | ECHO);          // ICANON: 캐논 모드 끄기, ECHO: 에코 끄기
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);   // 새 설정 적용
    
    char ch;
    while (true) {
        ch = getchar();                        // 문자 하나 읽기 (즉시)
        
        if (ch == '\n' || ch == '\r') {        // Enter 키
            break;                             // 입력 완료
        } else if (ch == 127 || ch == 8) {    // Backspace (127=DEL, 8=BS)
            if (!password.empty()) {           // 비밀번호가 비어있지 않으면
                password.pop_back();           // 마지막 문자 삭제
                std::cout << "\b \b" << std::flush;  // 화면에서 * 지우기
                // \b: 커서 왼쪽 이동, 공백: 문자 덮어쓰기, \b: 다시 왼쪽 이동
            }
        } else if (ch >= 32 && ch <= 126) {    // 출력 가능한 문자 (공백~틸드)
            password += ch;                    // 비밀번호에 추가
            std::cout << '*' << std::flush;    // 즉시 * 출력
        }
        // 그 외 제어 문자는 무시
    }
    std::cout << std::endl;                    // 줄바꿈
    
    // 터미널 설정 복구
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    
    return password;                           // 입력된 비밀번호 반환
}

// ============================================================================
// AuthClient 클래스
// 설명: 인증 및 FTP 클라이언트
// ============================================================================
class AuthClient {
private:
    std::string server_ip;      // 서버 IP 주소
    int server_port;            // 서버 포트 번호

    // ========================================================================
    // 요청 전송 함수 (JSON 요청/응답)
    // 설명: 서버에 JSON 요청을 보내고 응답을 받음
    // 매개변수: request - 보낼 JSON 요청
    //          response - 받을 JSON 응답 (참조)
    // 반환값: true (성공) / false (실패)
    // ========================================================================
    bool sendRequest(const json& request, json& response) {
        // 소켓 생성
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "소켓 생성 실패" << std::endl;
            return false;
        }

        // 서버 주소 구조체 설정
        struct sockaddr_in server_address;
        server_address.sin_family = AF_INET;                        // IPv4
        server_address.sin_port = htons(server_port);               // 포트 번호 (호스트→네트워크 바이트 순서)

        // IP 주소 문자열을 바이너리로 변환
        if (inet_pton(AF_INET, server_ip.c_str(), &server_address.sin_addr) <= 0) {
            std::cerr << "잘못된 주소" << std::endl;
            close(sock);
            return false;
        }

        // 서버에 연결
        if (connect(sock, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
            std::cerr << "서버 연결 실패" << std::endl;
            close(sock);
            return false;
        }

        // JSON 요청을 문자열로 변환
        std::string request_str = request.dump();
        
        // 서버로 전송
        send(sock, request_str.c_str(), request_str.length(), 0);

        // 서버 응답 수신
        char buffer[8192] = {0};                                    // 수신 버퍼 (8KB)
        int bytes_read = read(sock, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';                              // 문자열 끝 표시
            try {
                response = json::parse(buffer);                     // JSON 파싱
                close(sock);                                        // 소켓 닫기
                return true;
            } catch (json::parse_error& e) {
                std::cerr << "응답 파싱 실패" << std::endl;
                close(sock);
                return false;
            }
        }

        close(sock);
        return false;
    }

public:
    // 생성자: 서버 정보 초기화
    AuthClient(const std::string& ip, int port) 
        : server_ip(ip), server_port(port) {}

    // ========================================================================
    // 회원가입 함수
    // 설명: 사용자로부터 정보를 입력받아 서버에 회원가입 요청
    // ========================================================================
    void registerUser() {
        std::cout << "\n========== 회원가입 ==========" << std::endl;
        
        json request;                                               // 요청 JSON 객체
        request["action"] = "register";                             // 액션 타입 설정

        std::string input;                                          // 입력 임시 저장

        // ----------------------------------------------------------------
        // 아이디 입력 (유효성 검사 루프)
        // ----------------------------------------------------------------
        while (true) {
            std::cout << "아이디 (4~20자): ";
            std::getline(std::cin, input);                          // 한 줄 입력
            
            // 길이 체크 (4~20자)
            if (input.length() >= 4 && input.length() <= 20) {
                request["user_id"] = input;                         // JSON에 추가
                break;                                              // 루프 탈출
            } else {
                // 조건 불만족 → 다시 입력
                std::cout << "❌ 아이디는 4~20자 사이여야 합니다. 다시 입력하세요.\n" << std::endl;
            }
        }

        // ----------------------------------------------------------------
        // 비밀번호 입력 (유효성 검사 루프)
        // ----------------------------------------------------------------
        while (true) {
            std::cout << "비밀번호 (8자 이상, 대소문자/숫자/특수문자 포함): ";
            std::string password = getPasswordInput();              // 마스킹 입력
            
            // 복잡도 체크
            if (isValidPassword(password)) {
                request["password"] = password;                     // JSON에 추가
                break;
            } else {
                std::cout << "❌ 비밀번호는 8자 이상이며, 대소문자/숫자/특수문자를 모두 포함해야 합니다.\n" << std::endl;
            }
        }

        // ----------------------------------------------------------------
        // 닉네임 입력 (유효성 검사 루프)
        // ----------------------------------------------------------------
        while (true) {
            std::cout << "닉네임 (2~15자): ";
            std::getline(std::cin, input);
            
            // 길이 체크 (2~15자)
            if (input.length() >= 2 && input.length() <= 15) {
                request["nickname"] = input;
                break;
            } else {
                std::cout << "❌ 닉네임은 2~15자 사이여야 합니다. 다시 입력하세요.\n" << std::endl;
            }
        }

        // ----------------------------------------------------------------
        // 이메일 입력 (유효성 검사 루프)
        // ----------------------------------------------------------------
        while (true) {
            std::cout << "이메일: ";
            std::getline(std::cin, input);
            
            // 이메일 형식 체크
            if (isValidEmail(input)) {
                request["email"] = input;
                break;
            } else {
                std::cout << "❌ 올바른 이메일 형식이 아닙니다. (예: user@example.com)\n" << std::endl;
            }
        }

        // ----------------------------------------------------------------
        // 전화번호 입력 (유효성 검사 루프)
        // ----------------------------------------------------------------
        while (true) {
            std::cout << "전화번호 (예: 010-1234-5678): ";
            std::getline(std::cin, input);
            
            // 전화번호 형식 체크
            if (isValidPhoneNumber(input)) {
                request["phone"] = input;
                break;
            } else {
                std::cout << "❌ 전화번호 형식이 올바르지 않습니다. (예: 010-1234-5678)\n" << std::endl;
            }
        }

        // ----------------------------------------------------------------
        // 성별 입력 (유효성 검사 루프)
        // ----------------------------------------------------------------
        while (true) {
            std::cout << "성별 (M: 남성, F: 여성, O: 기타): ";
            std::getline(std::cin, input);
            
            // M, F, O 중 하나인지 체크
            if (input == "M" || input == "F" || input == "O") {
                request["gender"] = input;
                break;
            } else {
                std::cout << "❌ 성별은 M, F, O 중 하나를 입력해야 합니다.\n" << std::endl;
            }
        }

        // ----------------------------------------------------------------
        // 나이 입력 (유효성 검사 루프)
        // ----------------------------------------------------------------
        while (true) {
            std::cout << "나이: ";
            std::getline(std::cin, input);
            
            try {
                int age = std::stoi(input);                         // 문자열을 정수로 변환
                // 나이 범위 체크 (1~150)
                if (age >= 1 && age <= 150) {
                    request["age"] = age;
                    break;
                } else {
                    std::cout << "❌ 나이는 1~150 사이여야 합니다.\n" << std::endl;
                }
            } catch (...) {                                         // 변환 실패 (예외 처리)
                std::cout << "❌ 올바른 숫자를 입력하세요.\n" << std::endl;
            }
        }

        std::cout << "\n회원가입 요청 중..." << std::endl;

        // ----------------------------------------------------------------
        // 서버에 요청 전송 및 응답 처리
        // ----------------------------------------------------------------
        json response;
        if (sendRequest(request, response)) {
            bool success = response["success"];                     // 성공 여부
            std::string message = response["message"];              // 메시지

            // 결과 출력
            std::cout << "\n" << std::string(40, '=') << std::endl;
            std::cout << (success ? "✓ 성공" : "✗ 실패") << std::endl;
            std::cout << "메시지: " << message << std::endl;
            
            // 성공 시 추가 정보 출력
            if (success && response.contains("user_id")) {
                std::cout << "등록된 아이디: " << response["user_id"] << std::endl;
                std::cout << "닉네임: " << response["nickname"] << std::endl;
            }
            std::cout << std::string(40, '=') << std::endl;
        } else {
            std::cout << "서버 통신 실패" << std::endl;
        }
    }

    // ========================================================================
    // 로그인 함수
    // 설명: 아이디와 비밀번호를 입력받아 서버에 로그인 요청
    // ========================================================================
    void loginUser() {
        std::cout << "\n========== 로그인 ==========" << std::endl;

        json request;
        request["action"] = "login";                                // 액션 타입 설정

        // 아이디 입력
        std::string user_id;
        std::cout << "아이디: ";
        std::getline(std::cin, user_id);
        request["user_id"] = user_id;

        // 비밀번호 입력 (마스킹)
        std::cout << "비밀번호: ";
        std::string password = getPasswordInput();
        request["password"] = password;

        std::cout << "\n로그인 요청 중..." << std::endl;

        // 서버에 요청 전송 및 응답 처리
        json response;
        if (sendRequest(request, response)) {
            bool success = response["success"];
            std::string message = response["message"];

            // 결과 출력
            std::cout << "\n" << std::string(40, '=') << std::endl;
            std::cout << (success ? "✓ 성공" : "✗ 실패") << std::endl;
            std::cout << "메시지: " << message << std::endl;

            // 성공 시 사용자 정보 출력
            if (success && response.contains("user_info")) {
                auto user_info = response["user_info"];
                std::cout << "\n사용자 정보:" << std::endl;
                std::cout << "  ID: " << user_info["id"] << std::endl;
                std::cout << "  아이디: " << user_info["user_id"] << std::endl;
                std::cout << "  닉네임: " << user_info["nickname"] << std::endl;
                std::cout << "  이메일: " << user_info["email"] << std::endl;
                std::cout << "  전화번호: " << user_info["phone"] << std::endl;
                std::cout << "  성별: " << user_info["gender"] << std::endl;
                std::cout << "  나이: " << user_info["age"] << std::endl;
                std::cout << "  가입일: " << user_info["created_at"] << std::endl;
            }
            std::cout << std::string(40, '=') << std::endl;
        } else {
            std::cout << "서버 통신 실패" << std::endl;
        }
    }

    // ========================================================================
    // 메인 메뉴 함수
    // 설명: 사용자에게 메뉴를 보여주고 선택 처리
    // ========================================================================
    void showMenu() {
        while (true) {                                              // 무한 루프 (종료 선택 시까지)
            // 메뉴 출력
            std::cout << "\n╔════════════════════════════════╗" << std::endl;
            std::cout << "║     인증 시스템 클라이언트     ║" << std::endl;
            std::cout << "╠════════════════════════════════╣" << std::endl;
            std::cout << "║  1. 회원가입                   ║" << std::endl;
            std::cout << "║  2. 로그인                     ║" << std::endl;
            std::cout << "║  3. 파일 업로드 (FTP)          ║" << std::endl;
            std::cout << "║  4. 파일 다운로드 (FTP)        ║" << std::endl;
            std::cout << "║  5. 파일 목록 보기             ║" << std::endl;
            std::cout << "║  6. 종료                       ║" << std::endl;
            std::cout << "╚════════════════════════════════╝" << std::endl;
            std::cout << "선택: ";

            std::string choice;
            std::getline(std::cin, choice);                         // 선택 입력

            // 선택에 따라 해당 함수 호출
            if (choice == "1") {
                registerUser();                                     // 회원가입
            } else if (choice == "2") {
                loginUser();                                        // 로그인
            } else if (choice == "3") {
                uploadFile();                                       // 파일 업로드
            } else if (choice == "4") {
                downloadFile();                                     // 파일 다운로드
            } else if (choice == "5") {
                listFiles();                                        // 파일 목록
            } else if (choice == "6") {
                std::cout << "프로그램을 종료합니다." << std::endl;
                break;                                              // 루프 탈출 (프로그램 종료)
            } else {
                std::cout << "잘못된 선택입니다." << std::endl;
            }
        }
    }

    // ========================================================================
    // 파일 업로드 함수
    // 설명: 로컬 파일을 서버로 업로드
    // ========================================================================
    void uploadFile() {
        std::cout << "\n========== 파일 업로드 ==========" << std::endl;
        
        std::string filepath;
        std::cout << "업로드할 파일 경로: ";
        std::getline(std::cin, filepath);

        // ----------------------------------------------------------------
        // 파일 열기 및 크기 확인
        // ----------------------------------------------------------------
        // ios::binary: 바이너리 모드 (텍스트 변환 방지)
        // ios::ate: 파일 끝으로 이동 (크기 확인용)
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {                                      // 파일 열기 실패
            std::cout << "❌ 파일을 열 수 없습니다: " << filepath << std::endl;
            return;
        }

        // 파일 크기 확인
        size_t filesize = file.tellg();                             // 현재 위치 = 파일 크기
        file.seekg(0, std::ios::beg);                               // 파일 시작으로 이동

        // ----------------------------------------------------------------
        // 파일명 추출 (경로에서 파일명만)
        // ----------------------------------------------------------------
        size_t pos = filepath.find_last_of("/\\");                  // 마지막 / 또는 \ 찾기
        std::string filename = (pos == std::string::npos) ? filepath : filepath.substr(pos + 1);
        // 예: "/home/user/test.txt" → "test.txt"

        std::cout << "파일명: " << filename << std::endl;
        std::cout << "크기: " << filesize << " bytes" << std::endl;

        // ----------------------------------------------------------------
        // 서버에 연결
        // ----------------------------------------------------------------
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cout << "❌ 소켓 생성 실패" << std::endl;
            return;
        }

        struct sockaddr_in server_address;
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip.c_str(), &server_address.sin_addr);

        if (connect(sock, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
            std::cout << "❌ 서버 연결 실패" << std::endl;
            close(sock);
            return;
        }

        // ----------------------------------------------------------------
        // 업로드 요청 전송 (JSON)
        // ----------------------------------------------------------------
        json request;
        request["action"] = "upload";                               // 액션: 업로드
        request["filename"] = filename;                             // 파일명
        request["filesize"] = filesize;                             // 파일 크기

        std::string request_str = request.dump();
        send(sock, request_str.c_str(), request_str.length(), 0);

        // ----------------------------------------------------------------
        // 서버 응답 대기 (준비 완료 확인)
        // ----------------------------------------------------------------
        char buffer[4096] = {0};
        recv(sock, buffer, sizeof(buffer) - 1, 0);
        
        try {
            json response = json::parse(buffer);
            if (!response["success"]) {                             // 서버 준비 실패
                std::cout << "❌ " << response["message"] << std::endl;
                close(sock);
                return;
            }
        } catch (...) {
            std::cout << "❌ 서버 응답 오류" << std::endl;
            close(sock);
            return;
        }

        // ----------------------------------------------------------------
        // 파일 전송 (바이너리 안전)
        // ----------------------------------------------------------------
        const size_t CHUNK_SIZE = 4096;                             // 청크 크기: 4KB
        char file_buffer[CHUNK_SIZE];                               // 데이터 버퍼
        size_t total_sent = 0;                                      // 총 전송한 바이트 수

        std::cout << "\n파일 전송 중..." << std::endl;

        while (total_sent < filesize) {                             // 파일 크기만큼 반복
            size_t remaining = filesize - total_sent;               // 남은 바이트 수
            size_t to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;  // 읽을 크기
            
            // 파일에서 데이터 읽기
            file.read(file_buffer, to_read);
            std::streamsize bytes_read = file.gcount();             // 실제로 읽은 바이트 수 (중요!)
            
            if (bytes_read <= 0) {                                  // 파일 끝
                break;
            }
            
            // 읽은 만큼만 전송 (바이너리 안전)
            ssize_t sent = send(sock, file_buffer, bytes_read, 0);
            
            if (sent <= 0) {                                        // 전송 실패
                std::cout << "\n❌ 전송 중단" << std::endl;
                file.close();
                close(sock);
                return;
            }

            total_sent += sent;                                     // 누적 전송량

            // 진행률 표시
            int progress = (total_sent * 100) / filesize;
            std::cout << "\r진행률: " << progress << "% (" 
                      << total_sent << "/" << filesize << " bytes)" << std::flush;
        }

        std::cout << "\n✓ 파일 업로드 완료!" << std::endl;
        file.close();                                               // 파일 닫기
        close(sock);                                                // 소켓 닫기
    }

    // ========================================================================
    // 파일 다운로드 함수
    // 설명: 서버에서 파일을 다운로드
    // ========================================================================
    void downloadFile() {
        std::cout << "\n========== 파일 다운로드 ==========" << std::endl;
        
        std::string filename;
        std::cout << "다운로드할 파일명: ";
        std::getline(std::cin, filename);

        // ----------------------------------------------------------------
        // 서버에 연결
        // ----------------------------------------------------------------
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cout << "❌ 소켓 생성 실패" << std::endl;
            return;
        }

        struct sockaddr_in server_address;
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip.c_str(), &server_address.sin_addr);

        if (connect(sock, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
            std::cout << "❌ 서버 연결 실패" << std::endl;
            close(sock);
            return;
        }

        // ----------------------------------------------------------------
        // 다운로드 요청 전송 (JSON)
        // ----------------------------------------------------------------
        json request;
        request["action"] = "download";                             // 액션: 다운로드
        request["filename"] = filename;                             // 파일명

        std::string request_str = request.dump();
        send(sock, request_str.c_str(), request_str.length(), 0);

        // ----------------------------------------------------------------
        // 서버 응답 대기 (JSON 응답만 받기)
        // ----------------------------------------------------------------
        char response_buffer[8192] = {0};                           // 응답 버퍼 (8KB)
        ssize_t response_bytes = recv(sock, response_buffer, sizeof(response_buffer) - 1, 0);
        
        if (response_bytes <= 0) {                                  // 수신 실패
            std::cout << "❌ 서버 응답 수신 실패" << std::endl;
            close(sock);
            return;
        }

        size_t filesize = 0;
        try {
            // JSON 응답 파싱
            response_buffer[response_bytes] = '\0';                 // 문자열 끝 표시
            json response = json::parse(response_buffer);
            
            if (!response["success"]) {                             // 실패 응답
                std::cout << "❌ " << response["message"] << std::endl;
                close(sock);
                return;
            }
            filesize = response["filesize"];                        // 파일 크기
            std::cout << "파일 크기: " << filesize << " bytes" << std::endl;
        } catch (std::exception& e) {
            std::cout << "❌ 서버 응답 파싱 오류: " << e.what() << std::endl;
            close(sock);
            return;
        }

        // ----------------------------------------------------------------
        // 저장할 파일 경로 및 디렉토리 생성
        // ----------------------------------------------------------------
        std::string download_path = "./downloads/" + filename;      // 저장 경로
        
        // downloads 디렉토리 생성 (이미 있으면 무시)
        system("mkdir -p ./downloads");

        // 파일 열기 (바이너리 쓰기 모드)
        std::ofstream outfile(download_path, std::ios::binary);
        if (!outfile.is_open()) {
            std::cout << "❌ 파일 생성 실패" << std::endl;
            close(sock);
            return;
        }

        // ----------------------------------------------------------------
        // 파일 수신 (바이너리 안전)
        // ----------------------------------------------------------------
        const size_t CHUNK_SIZE = 4096;                             // 청크 크기: 4KB
        char file_buffer[CHUNK_SIZE];                               // 데이터 버퍼
        size_t total_received = 0;                                  // 총 수신한 바이트 수

        std::cout << "\n파일 다운로드 중..." << std::endl;

        while (total_received < filesize) {                         // 파일 크기만큼 반복
            size_t remaining = filesize - total_received;           // 남은 바이트 수
            size_t to_receive = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;  // 받을 크기
            
            // 소켓에서 데이터 수신
            ssize_t received = recv(sock, file_buffer, to_receive, 0);
            if (received <= 0) {                                    // 연결 끊김
                std::cout << "\n❌ 수신 중단 (받은 데이터: " 
                          << total_received << "/" << filesize << ")" << std::endl;
                outfile.close();
                close(sock);
                return;
            }

            // 받은 만큼만 파일에 쓰기
            outfile.write(file_buffer, received);
            total_received += received;                             // 누적 수신량

            // 진행률 표시
            int progress = (total_received * 100) / filesize;
            std::cout << "\r진행률: " << progress << "% (" 
                      << total_received << "/" << filesize << " bytes)" << std::flush;
        }

        std::cout << "\n✓ 파일 다운로드 완료!" << std::endl;
        std::cout << "저장 위치: " << download_path << std::endl;
        
        outfile.close();                                            // 파일 닫기
        close(sock);                                                // 소켓 닫기
    }

    // ========================================================================
    // 파일 목록 조회 함수
    // 설명: 서버에 있는 파일 목록을 조회
    // ========================================================================
    void listFiles() {
        std::cout << "\n========== 파일 목록 ==========" << std::endl;

        // JSON 요청 생성
        json request;
        request["action"] = "list_files";                           // 액션: 파일 목록

        // 서버에 요청 전송 및 응답 처리
        json response;
        if (sendRequest(request, response)) {
            if (response["success"]) {                              // 성공
                auto files = response["files"];                     // 파일 배열
                if (files.empty()) {                                // 파일이 없으면
                    std::cout << "업로드된 파일이 없습니다." << std::endl;
                } else {
                    std::cout << "\n총 " << files.size() << "개의 파일:" << std::endl;
                    int index = 1;
                    // 각 파일명 출력
                    for (const auto& file : files) {
                        std::cout << "  " << index++ << ". " << file << std::endl;
                    }
                }
            } else {                                                // 실패
                std::cout << "❌ " << response["message"] << std::endl;
            }
        } else {
            std::cout << "❌ 서버 통신 실패" << std::endl;
        }
    }
};

// ============================================================================
// main 함수 (프로그램 진입점)
// ============================================================================
int main() {
    // AuthClient 객체 생성 (서버 IP: 127.0.0.1, 포트: 8080)
    AuthClient client("127.0.0.1", 8080);
    
    // 메인 메뉴 표시 및 실행
    client.showMenu();
    
    return 0;                                                       // 프로그램 종료
}
