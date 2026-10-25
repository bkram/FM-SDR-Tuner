#ifndef SOCKET_COMPAT_H
#define SOCKET_COMPAT_H

#include <cstdint>

// A socket handle, in a type that is wide enough for SOCKET (UINT_PTR) on
// Win64 and for an int file descriptor on POSIX.
//
// The public headers deliberately do not include winsock2.h: it must not
// follow <windows.h> (which pulls in winsock 1), and audio_output.h needs
// <windows.h>. So a handle travels through the headers as an integer and is
// cast to the platform's socket type at the syscall sites.
using SocketHandle = std::intptr_t;

// The invalid-handle sentinel. INVALID_SOCKET is (SOCKET)(~0), which is -1
// read as a signed integer, and a POSIX descriptor that is not open is -1 as
// well: one value covers both platforms.
constexpr SocketHandle kInvalidSocket = -1;

#endif
