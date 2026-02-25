cmake_minimum_required(VERSION 3.16)                                            # // CMake 최소 버전
project(3loud LANGUAGES C CXX)                                                   # // 프로젝트 이름 및 언어

set(CMAKE_C_STANDARD 11)                                                        # // C 표준
set(CMAKE_CXX_STANDARD 17)                                                      # // C++ 표준
set(CMAKE_CXX_STANDARD_REQUIRED ON)                                              # // C++ 표준 강제

if(NOT CMAKE_BUILD_TYPE)                                                        # // 빌드 타입 미지정이면
    set(CMAKE_BUILD_TYPE Release)                                                # // Release 기본값
endif()                                                                         # // if 끝

add_compile_options(-Wall -Wextra -Wpedantic)                                   # // 경고 옵션

find_package(Threads REQUIRED)                                                   # // 스레드 라이브러리 탐색
find_package(CURL REQUIRED)                                                      # // libcurl 탐색(이메일 기능)
find_package(OpenSSL REQUIRED)                                                   # // OpenSSL 탐색(해시/암호 등)

find_package(nlohmann_json QUIET)                                                # // nlohmann/json 탐색(있으면 타겟으로 링크)
if(NOT nlohmann_json_FOUND)                                                      # // 패키지로 못 찾았으면
    find_path(NLOHMANN_JSON_INCLUDE_DIR                                          # // 헤더 경로 직접 탐색
        NAMES nlohmann/json.hpp                                                  # // json.hpp 찾기
        PATHS /usr/include /usr/local/include                                    # // 탐색 경로
    )                                                                            # // find_path 끝
    if(NOT NLOHMANN_JSON_INCLUDE_DIR)                                            # // 그래도 없으면
        message(FATAL_ERROR                                                      # // 종료
            "nlohmann/json 을 찾을 수 없습니다.\n"                                # // 에러 메시지
            "설치: sudo apt install nlohmann-json3-dev"                          # // 설치 안내
        )                                                                        # // message 끝
    endif()                                                                      # // if 끝
endif()                                                                          # // if 끝

find_library(MARIADB_CPP_LIB                                                     # // MariaDB C++ 커넥터 라이브러리 탐색
    NAMES mariadbcpp                                                             # // 라이브러리 이름
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu                      # // 탐색 경로
)                                                                                # // find_library 끝
find_path(MARIADB_CPP_INCLUDE_DIR                                                # // MariaDB C++ 커넥터 헤더 탐색
    NAMES mariadb/conncpp.hpp                                                    # // 헤더 파일
    PATHS /usr/include /usr/local/include                                        # // 탐색 경로
)                                                                                # // find_path 끝
if(NOT MARIADB_CPP_LIB OR NOT MARIADB_CPP_INCLUDE_DIR)                           # // 둘 중 하나라도 없으면
    message(FATAL_ERROR                                                          # // 종료
        "MariaDB C++ Connector 를 찾을 수 없습니다.\n"                            # // 에러 메시지
        "예: sudo apt install libmariadb-dev\n"                                  # // 설치 안내(환경 따라 다름)
        "mariadb-connector-cpp 별도 설치가 필요할 수 있습니다."                  # // 주의 안내
    )                                                                            # // message 끝
endif()                                                                          # // if 끝

set(PROTOCOL_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/protocol)                           # // protocol 폴더 include 경로

add_executable(3loud_server                                                      # // 서버 타겟
    server/skeleton_server.cpp                                                   # // 서버 메인 루프(epoll/worker)
    server/email.cpp                                                             # // 이메일 발송 구현(auth에서 필요)
    server_handle/file_handler.cpp                                               # // 파일 핸들러(네 구조 유지)
    protocol/packet.c                                                            # // length-prefix C 모듈
)                                                                                # // add_executable 끝

target_include_directories(3loud_server PRIVATE                                   # // 서버 include 경로
    ${PROTOCOL_INCLUDE_DIR}                                                      # // protocol 헤더들
    ${CMAKE_SOURCE_DIR}/server                                                   # // server 헤더들(email.h 등)
    ${CMAKE_SOURCE_DIR}/server_handle                                            # // server_handle 헤더들
    ${MARIADB_CPP_INCLUDE_DIR}                                                   # // MariaDB include
)                                                                                # // include dirs 끝

target_link_libraries(3loud_server PRIVATE                                        # // 서버 링크 라이브러리
    ${MARIADB_CPP_LIB}                                                           # // MariaDB C++ 커넥터
    CURL::libcurl                                                                # // curl 링크
    OpenSSL::SSL                                                                 # // OpenSSL SSL
    OpenSSL::Crypto                                                              # // OpenSSL Crypto
    Threads::Threads                                                             # // pthread
    dl                                                                           # // dl(일부 환경에서 필요)
)                                                                                # // link libs 끝

if(nlohmann_json_FOUND)                                                          # // 패키지로 찾았으면
    target_link_libraries(3loud_server PRIVATE nlohmann_json::nlohmann_json)      # // 타겟 링크
else()                                                                           # // 아니면
    target_include_directories(3loud_server PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR}) # // include 경로 추가
endif()                                                                          # // if 끝

add_executable(3loud_client
    client/skeleton_client.cpp                 # // 메인 UI 루프
    client/client_net.cpp                      # // 네트워크
    client/client_handlers.cpp                 # // auth 핸들러(요청 생성 등)
    client_handle/file_client.cpp              # // 파일 기능 구현(handle_file_*)
    client_handle/client_handlers_stub.cpp     # // logout 등 미구현 스텁(링크 해결용)
    protocol/packet.c                          # // length-prefix 모듈
)                                           # // 타겟 끝

target_include_directories(3loud_client PRIVATE
    ${PROTOCOL_INCLUDE_DIR}                    # // protocol 헤더 경로
    ${CMAKE_SOURCE_DIR}/client                 # // client 헤더 경로
    ${CMAKE_SOURCE_DIR}/client_handle          # // client_handle 헤더 경로
)                                              

target_link_libraries(3loud_client PRIVATE                                        # // 클라 링크 라이브러리
    Threads::Threads                                                             # // pthread
    OpenSSL::SSL                                                                 # // OpenSSL SSL
    OpenSSL::Crypto                                                              # // OpenSSL Crypto
)                                                                                # // link libs 끝

if(nlohmann_json_FOUND)                                                          # // 패키지로 찾았으면
    target_link_libraries(3loud_client PRIVATE nlohmann_json::nlohmann_json)      # // 타겟 링크
else()                                                                           # // 아니면
    target_include_directories(3loud_client PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR}) # // include 경로 추가
endif()                                                                          # // if 끝