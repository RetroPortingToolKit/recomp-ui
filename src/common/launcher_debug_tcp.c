/* Opt-in loopback control for the hidden launcher. One UTF-8 command per line,
 * one JSON reply per command. Polled on the UI thread; never blocks a frame. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET DebugSocket;
#define BAD_SOCKET INVALID_SOCKET
#define socket_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int DebugSocket;
#define BAD_SOCKET (-1)
#define socket_close close
#endif
#include "launcher_debug_tcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static DebugSocket listener = BAD_SOCKET, peer = BAD_SOCKET;
static char input[2048], output[4096];
static size_t used, output_size, output_sent;
#ifdef _WIN32
static bool winsock_ready;
#endif
static bool would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
static bool nonblocking(DebugSocket s) {
#ifdef _WIN32
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}
static void drop_peer(void) {
    if (peer != BAD_SOCKET) socket_close(peer);
    peer = BAD_SOCKET;
    used = output_size = output_sent = 0;
}
void launcher_debug_tcp_close(void) {
    drop_peer();
    if (listener != BAD_SOCKET) socket_close(listener);
    listener = BAD_SOCKET;
#ifdef _WIN32
    if (winsock_ready) WSACleanup();
    winsock_ready = false;
#endif
}
bool launcher_debug_tcp_active(void) { return listener != BAD_SOCKET; }
bool launcher_debug_tcp_open(const char* value) {
    launcher_debug_tcp_close();
    char* end = NULL;
    unsigned long port = value ? strtoul(value, &end, 10) : 65536;
    if (!value || !value[0] || !end || *end || port > 65535) goto fail;
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) goto fail;
    winsock_ready = true;
#endif
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == BAD_SOCKET || !nonblocking(listener)) goto fail;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) ||
        listen(listener, 1)) goto fail;
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(listener, (struct sockaddr*)&address, &length)) goto fail;
    fprintf(stderr, "[dbg] hidden TCP listening on 127.0.0.1:%u\n", ntohs(address.sin_port));
    const char* file = getenv("LNG_TCP_PORT_FILE");
    if (file && file[0]) {
        FILE* f = fopen(file, "w");
        if (!f) goto fail;
        fprintf(f, "%u\n", ntohs(address.sin_port));
        if (fclose(f)) goto fail;
    }
    return true;
fail:
    fprintf(stderr, "[dbg] cannot open hidden TCP control socket\n");
    launcher_debug_tcp_close();
    return false;
}
static void flush_reply(void) {
    if (peer == BAD_SOCKET || output_sent >= output_size) return;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    int n = (int)send(peer, output + output_sent, (int)(output_size - output_sent), flags);
    if (n > 0) output_sent += (size_t)n;
    else if (n < 0 && !would_block()) drop_peer();
}
void launcher_debug_tcp_reply(const char* json) {
    if (peer == BAD_SOCKET) return;
    output_size = (size_t)snprintf(output, sizeof(output), "%s\n", json);
    if (output_size >= sizeof(output)) {
        strcpy(output, "{\"ok\":false,\"error\":\"reply_too_large\"}\n");
        output_size = strlen(output);
    }
    output_sent = 0;
    flush_reply();
}
int launcher_debug_tcp_command(char* out, size_t cap) {
    if (listener == BAD_SOCKET) return 0;
    if (peer == BAD_SOCKET) {
        peer = accept(listener, NULL, NULL);
        if (peer == BAD_SOCKET) return 0;
        if (!nonblocking(peer)) { drop_peer(); return 0; }
    }
    flush_reply();
    if (peer == BAD_SOCKET) return 0;
    if (output_sent < output_size) return 0;
    char* newline = memchr(input, '\n', used);
    if (!newline) {
        int n = (int)recv(peer, input + used, (int)(sizeof(input) - used), 0);
        if (n == 0) { drop_peer(); return 0; }
        if (n < 0 && !would_block()) { drop_peer(); return 0; }
        if (n > 0) used += (size_t)n;
        newline = memchr(input, '\n', used);
    }
    if (!newline) {
        if (used == sizeof(input)) { drop_peer(); return -1; }
        return 0;
    }
    size_t bytes = (size_t)(newline - input), consumed = bytes + 1;
    if (bytes && input[bytes - 1] == '\r') --bytes;
    if (bytes >= cap || memchr(input, '\0', bytes)) {
        memmove(input, input + consumed, used - consumed); used -= consumed;
        return -1;
    }
    memcpy(out, input, bytes); out[bytes] = '\0';
    memmove(input, input + consumed, used - consumed); used -= consumed;
    return 1;
}
