#include "packet.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int send_all(int sock, const char* buf, uint32_t len)
{
    uint32_t total_sent = 0;
    while (total_sent < len) {
        int n = send(sock, buf + total_sent, len - total_sent, 0);
        if (n <= 0) return -1;
        total_sent += n;
    }
    return 0;
}

static int recv_all(int sock, char* buf, uint32_t len)
{
    uint32_t total_recv = 0;
    while (total_recv < len) {
        int n = recv(sock, buf + total_recv, len - total_recv, 0);
        if (n <= 0) return -1;
        total_recv += n;
    }
    return 0;
}

int packet_send(int sock, const char* data, uint32_t len)
{
    uint32_t net_len = htonl(len);
    if (send_all(sock, (char*)&net_len, sizeof(net_len)) < 0) return -1;
    if (send_all(sock, data, len) < 0) return -1;
    return 0;
}

int packet_recv(int sock, char** out_buf, uint32_t* out_len)
{
    uint32_t net_len;
    if (recv_all(sock, (char*)&net_len, sizeof(net_len)) < 0) return -1;

    uint32_t len = ntohl(net_len);
    char* buf = (char*)malloc(len + 1);
    if (!buf) return -1;

    if (recv_all(sock, buf, len) < 0) { free(buf); return -1; }

    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}