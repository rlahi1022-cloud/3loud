cmake_minimum_required(VERSION 3.10)
project(3loud)

# ==========================================================
# 1. C/C++ 표준
# ==========================================================
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==========================================================
# 2. Include 경로
# ==========================================================
include_directories(
    ${CMAKE_SOURCE_DIR}/protocol
    ${CMAKE_SOURCE_DIR}/client
    ${CMAKE_SOURCE_DIR}/client_handle
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/server_handle
)

# ==========================================================
# 3. 필수 패키지
# ==========================================================
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# MariaDB C++ Connector
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

include_directories(${MARIADB_CPP_INCLUDE_DIR})

# ==========================================================
# 4. Protocol 라이브러리 (공용)
# ==========================================================
add_library(protocol_lib
    protocol/packet.c
)

# ==========================================================
# 5. Server 빌드
# ==========================================================
add_executable(server_app
    server/skeleton_server.cpp
    server/email.cpp
    server_handle/file_handler.cpp
    server_handle/message_handler.cpp
)

target_link_libraries(server_app
    protocol_lib
    Threads::Threads
    ${CURL_LIBRARIES}
    ${MARIADB_CPP_LIB}
)

# ==========================================================
# 6. Client 빌드
# ==========================================================
add_executable(client_app
    client/skeleton_client.cpp
    client/client_net.cpp
    client/client_handlers.cpp
    client_handle/client_handlers_stub.cpp
    client_handle/file_client.cpp
    client_handle/client_messagehandler.cpp
)

target_link_libraries(client_app
    protocol_lib
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)