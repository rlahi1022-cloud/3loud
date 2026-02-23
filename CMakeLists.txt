cmake_minimum_required(VERSION 3.16)
project(3loud LANGUAGES C CXX)

# ── C/C++ 표준 설정 ──────────────────────────────────────────────
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 빌드 타입 기본값 (미지정 시 Release) ─────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── 컴파일 옵션 ──────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic)

# ── 외부 라이브러리 탐색 ─────────────────────────────────────────

# nlohmann/json (헤더 전용)
# 없으면: sudo apt install nlohmann-json3-dev
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    # 설치 안 됐을 경우 직접 경로 지정 (시스템 기본 경로에서 탐색)
    find_path(NLOHMANN_JSON_INCLUDE_DIR
        NAMES nlohmann/json.hpp
        PATHS /usr/include /usr/local/include
    )
    if(NLOHMANN_JSON_INCLUDE_DIR)
        message(STATUS "nlohmann/json found at: ${NLOHMANN_JSON_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR
            "nlohmann/json 을 찾을 수 없습니다.\n"
            "설치 명령: sudo apt install nlohmann-json3-dev")
    endif()
endif()

# MariaDB C++ Connector
# 없으면: sudo apt install libmariadb-dev  +  별도 mariadb-connector-cpp 설치
find_library(MARIADB_CPP_LIB
    NAMES mariadbcpp
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)
find_path(MARIADB_CPP_INCLUDE_DIR
    NAMES mariadb/conncpp.hpp
    PATHS /usr/include /usr/local/include
)
if(NOT MARIADB_CPP_LIB OR NOT MARIADB_CPP_INCLUDE_DIR)
    message(FATAL_ERROR
        "MariaDB C++ Connector 를 찾을 수 없습니다.\n"
        "설치 명령:\n"
        "  sudo apt install libmariadb-dev\n"
        "  (mariadb-connector-cpp 별도 빌드 필요 시 README 참고)")
endif()
message(STATUS "MariaDB C++ lib : ${MARIADB_CPP_LIB}")
message(STATUS "MariaDB include : ${MARIADB_CPP_INCLUDE_DIR}")

# ── 공통 헤더 경로 ────────────────────────────────────────────────
# protocol/ 폴더: packet.h, protocal.h, json_packet.hpp
set(PROTOCOL_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/protocol)

# ════════════════════════════════════════════════════════════════
#  서버 실행 파일: 3loud_server
# ════════════════════════════════════════════════════════════════
add_executable(3loud_server
    server/skeleton_server.cpp          # 메인 루프 (epoll + worker)
    server_handle/file_handler.cpp      # 파일 핸들러
    protocol/packet.c                   # C 공용 length-prefix 모듈
)

target_include_directories(3loud_server PRIVATE
    ${PROTOCOL_INCLUDE_DIR}             # packet.h, protocal.h, json_packet.hpp
    ${CMAKE_SOURCE_DIR}/server_handle   # file_handler.hpp
    ${MARIADB_CPP_INCLUDE_DIR}
)

target_link_libraries(3loud_server PRIVATE
    ${MARIADB_CPP_LIB}
    pthread
)

# nlohmann/json 연결
if(nlohmann_json_FOUND)
    target_link_libraries(3loud_server PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(3loud_server PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

# ════════════════════════════════════════════════════════════════
#  클라이언트 실행 파일: 3loud_client
# ════════════════════════════════════════════════════════════════
add_executable(3loud_client
    client/skeleton_client.cpp          # 메인 UI 루프
    client/client_net.cpp               # send_json / recv_json
    client_handle/file_client.cpp       # 파일 메뉴 핸들러
    client_handle/client_handlers_stub.cpp
    protocol/packet.c                   # C 공용 length-prefix 모듈
)

target_include_directories(3loud_client PRIVATE
    ${PROTOCOL_INCLUDE_DIR}             # packet.h, protocal.h, json_packet.hpp
    ${CMAKE_SOURCE_DIR}/client          # client_net.hpp (file_client.cpp가 참조)
    ${CMAKE_SOURCE_DIR}/client_handle   # file_client.hpp
)

target_link_libraries(3loud_client PRIVATE
    pthread
)

# nlohmann/json 연결
if(nlohmann_json_FOUND)
    target_link_libraries(3loud_client PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(3loud_client PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
