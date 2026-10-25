#ifndef SOCKET_SUBSYSTEM_H
#define SOCKET_SUBSYSTEM_H

// On Windows the socket subsystem (winsock) has to be started before the
// first socket call and shut down when the last user is done with it.
// WSAStartup and WSACleanup are reference-counted by the OS, so every start
// has to be matched by exactly one cleanup.
//
// The three socket users in this project — the XDR server, the RTL-TCP
// client and the REST server — share one start: the helpers below keep the
// count.
//
// On Windows <winsock2.h> must be included before this header. It must not
// follow <windows.h>, which pulls in winsock 1, so the header does not
// include it itself; the check below turns a wrong include order into a
// clear error instead of a link failure.

#if defined(_WIN32) && !defined(_WINSOCK2API_)
#error "include <winsock2.h> before socket_subsystem.h"
#endif

#include <mutex>

class SocketSubsystem {
public:
  // Starts the subsystem if it is not running yet and takes a reference to
  // it. Returns false when the start failed.
  static bool acquire() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex());
    if (refs() == 0) {
      WSADATA wsaData;
      if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
      }
    }
    refs()++;
    return true;
#else
    return true;
#endif
  }

  // Gives a reference back. The subsystem is shut down when the last
  // reference is given back.
  static void release() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex());
    if (refs() > 0) {
      refs()--;
      if (refs() == 0) {
        WSACleanup();
      }
    }
#endif
  }

private:
#if defined(_WIN32)
  static std::mutex &mutex() {
    static std::mutex m;
    return m;
  }

  static int &refs() {
    static int r = 0;
    return r;
  }
#endif
};

// Holds one reference to the socket subsystem for as long as it is alive.
// The typical use is a function-local static, which releases the reference
// when the process exits.
class SocketSubsystemLease {
public:
  SocketSubsystemLease() : m_held(SocketSubsystem::acquire()) {}
  ~SocketSubsystemLease() {
    if (m_held) {
      SocketSubsystem::release();
    }
  }
  SocketSubsystemLease(const SocketSubsystemLease &) = delete;
  SocketSubsystemLease &operator=(const SocketSubsystemLease &) = delete;

  // False when the subsystem could not be started.
  bool ok() const { return m_held; }

private:
  bool m_held;
};

#endif
