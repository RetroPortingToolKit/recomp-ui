#ifndef RECOMP_LAUNCHER_DEBUG_TCP_H
#define RECOMP_LAUNCHER_DEBUG_TCP_H
#include <stdbool.h>
#include <stddef.h>
bool launcher_debug_tcp_open(const char* port);
bool launcher_debug_tcp_active(void);
int launcher_debug_tcp_command(char* out, size_t cap);
void launcher_debug_tcp_reply(const char* json);
void launcher_debug_tcp_close(void);
#endif
