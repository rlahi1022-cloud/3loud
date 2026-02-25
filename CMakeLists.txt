cmake_minimum_required(VERSION 3.10)
project(3loud)

# C++ 표준 설정
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# 헤더 경로 포함
include_directories(
    ${CMAKE_SOURCE_DIR}/protocol
    ${CMAKE_SOURCE_DIR}/client
    ${CMAKE_SOURCE_DIR}/client_handle
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/server_handle
)

# ==========================================
# 1. 필수 패키지 찾기 (Threads, CURL, OpenSSL)
# ==========================================
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)  # <--- [추가됨] OpenSSL 찾기

# ==========================================
# 2. 라이브러리 빌드 (Protocol)
# ==========================================
add_library(protocol_lib
    protocol/packet.c
    protocol/packet.h
    protocol/protocol.h
    protocol/protocol_schema.h
    protocol/json_packet.hpp
)

# ==========================================
# 3. Server 빌드
# ==========================================
add_executable(server_app
    server/skeleton_server.cpp
    server/email.cpp
    server/server.h
    server_handle/file_handler.cpp
    server_handle/file_handler.hpp
)

# 서버는 DB와 CURL이 필요함
target_link_libraries(server_app 
    protocol_lib 
    Threads::Threads 
    ${CURL_LIBRARIES}
    mariadbcpp
)

# ==========================================
# 4. Client 빌드 (에러 났던 곳)
# ==========================================
add_executable(client_app
    client/skeleton_client.cpp
    client/client_net.cpp
    client/client_handlers.cpp
    client_handle/file_client.cpp
    client_handle/client_handlers_stub.cpp
)

# [수정됨] 클라이언트는 OpenSSL(암호화)이 필요함
target_link_libraries(client_app 
    protocol_lib 
    Threads::Threads 
    OpenSSL::SSL       # <--- [추가됨]
    OpenSSL::Crypto    # <--- [추가됨]
)
