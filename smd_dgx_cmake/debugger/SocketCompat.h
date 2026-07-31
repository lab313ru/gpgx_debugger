#pragma once
//
// The platform differences between the two ends of the bridge, in one place.
//
// Both the server and the client had their own copy of this block, which is how
// they came to share a bug: neither suppressed SIGPIPE, so on Linux a client
// that went away mid-reply killed the whole process — the emulator, or IDA with
// the user's database open, with no message.

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#  define CLOSESOCK closesocket
#  define BAD_SOCK  INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
using socket_t = int;
#  define CLOSESOCK ::close
#  define BAD_SOCK  (-1)
#endif

namespace sockcompat {

// Writing to a socket whose peer has gone raises SIGPIPE on POSIX, and the
// default disposition kills the process. Linux takes a per-call flag; the BSDs
// and macOS take a per-socket option; Windows has neither problem.
//
// Installing a process-wide SIG_IGN would be simpler and is deliberately NOT
// done: this code is linked into IDA, and changing the host's signal
// disposition behind its back is not ours to do.
#if defined(_WIN32) || !defined(MSG_NOSIGNAL)
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

inline void suppressSigpipe(socket_t fd)
{
#if defined(SO_NOSIGPIPE)          // macOS / BSD: the per-socket half
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

// Winsock needs process-wide initialisation. Done once and never torn down:
// WSACleanup is refcounted per process, so calling it when this component
// shuts down would pull the floor out from under any other socket user in the
// same process — IDA itself, or another plugin. The imbalance is deliberate.
inline bool netInit()
{
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

// Send the whole buffer. Returns false if the peer went away.
inline bool sendAll(socket_t fd, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const int w = ::send(fd, data + sent, int(len - sent), kSendFlags);
        if (w <= 0) return false;
        sent += size_t(w);
    }
    return true;
}

} // namespace sockcompat
