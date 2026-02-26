cmake_minimum_required(VERSION 3.16)
project(3loud LANGUAGES C CXX)

# ===============================
# 기본 설정
# ===============================
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

# ===============================
# 필수 패키지
# ===============================
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)   # ★ 추가

# ===============================
# MariaDB C++ Connector 찾기
# ===============================
find_library(MARIADB_CPP_LIB
    NAMES mariadbcpp
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu
)

find_path(MARIADB_CPP_INCLUDE_DIR
    NAMES mariadb/conncpp.hpp
    PATHS /usr/include
)

if(NOT MARIADB_CPP_LIB OR NOT MARIADB_CPP_INCLUDE_DIR)
    message(FATAL_ERROR "MariaDB C++ Connector not found")
endif()

# ===============================
# protocol 라이브러리
# ===============================
add_library(protocol_lib
    protocol/packet.c
)

target_include_directories(protocol_lib
    PUBLIC
        ${CMAKE_SOURCE_DIR}/protocol
)

# ===============================
# client_app
# ===============================
add_executable(client_app
    client/skeleton_client.cpp
    client/client_net.cpp
    client/client_handlers.cpp
    client_handle/client_messagehandler.cpp
    client_handle/file_client.cpp
    client_handle/file_settings.cpp
)

target_include_directories(client_app
    PRIVATE
        ${CMAKE_SOURCE_DIR}/client
        ${CMAKE_SOURCE_DIR}/client_handle
        ${CMAKE_SOURCE_DIR}/protocol
)

target_link_libraries(client_app
    PRIVATE
        protocol_lib
        Threads::Threads
        nlohmann_json::nlohmann_json
        OpenSSL::SSL          # ★ 추가
        OpenSSL::Crypto       # ★ 추가
)

# ===============================
# server_app
# ===============================
add_executable(server_app
    server/skeleton_server.cpp
    server/email.cpp
    server_handle/file_handler.cpp
    server_handle/message_handler.cpp
    server_handle/settings_handler.cpp
    server_handle/profile_handler.cpp
)

target_include_directories(server_app
    PRIVATE
        ${CMAKE_SOURCE_DIR}/server
        ${CMAKE_SOURCE_DIR}/server_handle
        ${CMAKE_SOURCE_DIR}/protocol
        ${MARIADB_CPP_INCLUDE_DIR}
)

target_link_libraries(server_app
    PRIVATE
        protocol_lib
        ${MARIADB_CPP_LIB}
        CURL::libcurl
        Threads::Threads
        nlohmann_json::nlohmann_json
        OpenSSL::SSL          # 서버에서도 혹시 사용할 수 있으니 같이 안정화
        OpenSSL::Crypto
)
