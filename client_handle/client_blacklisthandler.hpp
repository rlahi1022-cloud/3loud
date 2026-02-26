#ifndef CLIENT_BLACKLIST_HANDLER_HPP
#define CLIENT_BLACKLIST_HANDLER_HPP

// 블랙리스트 목록 조회
void handle_blacklist_list(int sock);

// 블랙리스트 추가
void handle_blacklist_add(int sock);

// 블랙리스트 삭제
void handle_blacklist_remove(int sock);

#endif