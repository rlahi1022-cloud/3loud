// ============================================================================
// 파일명: file_client.hpp
// 목적: skeleton_client.cpp에서 호출하는 파일 메뉴 핸들러 선언
//
// [기존 코드와의 연결]
//   client_handlers.h에 아래 선언들을 추가하거나,
//   skeleton_client.cpp 상단에 이 헤더를 include 하면 됨:
//     #include "file_client.hpp"
//
//   skeleton_client.cpp의 파일 서브메뉴:
//     if (sub == 1) { handle_file_list(sock);     continue; }
//     if (sub == 2) { handle_file_upload(sock);   continue; }
//     if (sub == 3) { handle_file_download(sock); continue; }
//     if (sub == 4) { handle_file_delete(sock);   continue; }  ← 삭제 추가
//
//   파일 서브메뉴 상단에 전송 중 상태 표시 추가 (요구사항 12-1-9):
//     if (g_file_transfer_in_progress.load())
//         std::cout << "  [파일 저장 중]\n";
//
//   user_no는 로그인 성공 시 전역 변수에 저장:
//     extern uint32_t g_user_no;
// ============================================================================
#pragma once

#include <cstdint>
#include <atomic>

// 전송 중 플래그 (멀티스레드 공유 - 요구사항 12-1-5, 12-1-6)
extern std::atomic<bool> g_file_transfer_in_progress;

// 업로드 전용 소켓 (메인 소켓과 분리하여 충돌 방지)
extern std::atomic<int> g_upload_sock;

// 업로드 진행률 (tui_menu footer 표시용)
extern std::atomic<int> g_upload_progress_pct;
extern std::atomic<int> g_upload_progress_cur;
extern std::atomic<int> g_upload_progress_tot;

// 로그인 후 설정되는 유저 no (skeleton_client.cpp에서 extern으로 선언)
extern uint32_t g_user_no;

// 업로드 전용 소켓 연결 (로그인 성공 직후 호출)
bool connect_upload_socket(const char* ip, int port);

// 파일 목록 조회 (0x0024)
void handle_file_list(int sock);

// 파일 업로드: 멀티스레드로 백그라운드 전송 (0x0020 + 0x0021)
void handle_file_upload(int sock);

// 파일 다운로드 (0x0022)
void handle_file_download(int sock);

// 파일 삭제 (0x0023)
void handle_file_delete(int sock);
