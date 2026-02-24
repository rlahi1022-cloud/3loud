#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int packet_send(int sock, const char* data, uint32_t len);
int packet_recv(int sock, char** out_buf, uint32_t* out_len);

#ifdef __cplusplus
}
#endif

#endif