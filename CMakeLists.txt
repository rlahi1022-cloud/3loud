cmake_minimum_required(VERSION 3.10)
project(3loud)

# C++ 17 표준 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD 99)

# ==========================================================
# [추가됨] OpenSSL 라이브러리 찾기 (SHA256 해싱용)
# ==========================================================
find_package(OpenSSL REQUIRED)

# ==========================================================
# 1. 헤더 파일 검색 경로 설정
# ==========================================================
include_directories(
    ${CMAKE_SOURCE_DIR}/protocol
    ${CMAKE_SOURCE_DIR}/client
    ${CMAKE_SOURCE_DIR}/client_handle
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/server_handle
    ${OPENSSL_INCLUDE_DIR} # [추가됨] OpenSSL 헤더 경로
)

# ==========================================================
# 2. 공통 프로토콜 라이브러리 타겟
# ==========================================================
add_library(protocol_lib STATIC
    protocol/packet.c
)

# ==========================================================
# 3. 클라이언트 실행 파일 타겟
# ==========================================================
set(CLIENT_SOURCES
    client/client_handlers.cpp
    client/client_net.cpp
    client/skeleton_client.cpp
    client_handle/admin_client.cpp
    client_handle/client_blacklisthandler.cpp
    client_handle/client_messagehandler.cpp
    client_handle/file_client.cpp
    client_handle/file_settings.cpp
)

add_executable(client_app ${CLIENT_SOURCES})

# [수정됨] 클라이언트 링킹에 OpenSSL::Crypto 추가
target_link_libraries(client_app protocol_lib pthread OpenSSL::Crypto)

# ==========================================================
# 4. 서버 실행 파일 타겟
# ==========================================================
set(SERVER_SOURCES
    server/email.cpp
    server/skeleton_server.cpp
    server_handle/blacklisthandler.cpp
    server_handle/file_handler.cpp
    server_handle/message_handler.cpp
    server_handle/profile_handler.cpp
    server_handle/settings_handler.cpp
    server_handle/admin_handler.cpp  
)

add_executable(server_app ${SERVER_SOURCES})

# [수정됨] 서버 링킹에도 안전하게 OpenSSL::Crypto 추가
target_link_libraries(server_app protocol_lib mariadbcpp curl pthread OpenSSL::Crypto)
