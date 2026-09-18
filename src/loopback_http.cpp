#include "loopback_http.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
using sock_t = SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
static bool sockValid(sock_t s) { return s != INVALID_SOCKET; }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
using sock_t = int;
static void closeSock(sock_t s) { ::close(s); }
static bool sockValid(sock_t s) { return s >= 0; }
#endif

std::string loopbackPost(int port, const std::string& path, const std::string& body, int timeoutMs) {
#ifdef _WIN32
    static const bool wsaReady = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!wsaReady) return {};
#endif
    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!sockValid(s)) return {};

#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeoutMs);
#else
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { closeSock(s); return {}; }

    const std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\nConnection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    if (::send(s, req.data(), static_cast<int>(req.size()), 0) != static_cast<int>(req.size())) { closeSock(s); return {}; }

    std::string resp;
    char buf[4096];
    for (;;) {
        const auto n = ::recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.size() > (1u << 20)) break;  // get_info is a few KB; cap anything else
    }
    closeSock(s);

    const auto sep = resp.find("\r\n\r\n");
    if (sep == std::string::npos || resp.compare(0, 12, "HTTP/1.1 200") != 0) return {};
    return resp.substr(sep + 4);
}
