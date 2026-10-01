#include "TcpStream.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error "TcpStream 目前只实现 Windows（与项目其余网络/窗口代码一致）"
#endif

#include <cstring>

namespace jc2::net {

namespace {

constexpr uintptr_t kInvalid = static_cast<uintptr_t>(~0ull);

void ensureWsa() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
            initialized = true;
        }
    }
}

std::string lastError(const char* what) {
    return std::string(what) + " failed (Winsock error " + std::to_string(WSAGetLastError()) + ")";
}

} // namespace

TcpStream::~TcpStream() {
    close();
}

bool TcpStream::connected() const {
    return sock_ != kInvalid;
}

void TcpStream::close() {
    if (sock_ != kInvalid) {
        closesocket(static_cast<SOCKET>(sock_));
        sock_ = kInvalid;
    }
}

bool TcpStream::connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err) {
    ensureWsa();
    timeoutMs_ = timeoutMs > 0 ? timeoutMs : 30000;

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
        err = "could not resolve host '" + host + "'";
        return false;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // 非阻塞 connect + select 超时
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        const int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = (rc == 0);
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(s, &writeSet);
            timeval tv;
            tv.tv_sec = timeoutMs_ / 1000;
            tv.tv_usec = (timeoutMs_ % 1000) * 1000;
            const int sel = select(0, nullptr, &writeSet, nullptr, &tv);
            if (sel > 0) {
                int soError = 0;
                int len = sizeof(soError);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len) == 0 &&
                    soError == 0) {
                    ok = true;
                } else {
                    err = "connection to " + host + ":" + portText + " refused";
                }
            } else if (sel == 0) {
                err = "connection to " + host + ":" + portText + " timed out";
            } else {
                err = lastError("select");
            }
        } else if (!ok) {
            err = lastError("connect");
        }

        if (!ok) {
            closesocket(s);
            continue;
        }

        // 回到阻塞模式并设置读写超时
        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        const DWORD tv = static_cast<DWORD>(timeoutMs_);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        // TLS 记录很小，禁用 Nagle 让往返更干脆
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        sock_ = static_cast<uintptr_t>(s);
        freeaddrinfo(result);
        return true;
    }

    freeaddrinfo(result);
    if (err.empty()) err = "could not connect to " + host + ":" + portText;
    return false;
}

bool TcpStream::send(const uint8_t* data, size_t len, std::string& err) {
    if (sock_ == kInvalid) {
        err = "socket is not connected";
        return false;
    }
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(data + sent),
                             static_cast<int>(len - sent), 0);
        if (n <= 0) {
            err = lastError("send");
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

ptrdiff_t TcpStream::recv(uint8_t* buf, size_t len, std::string& err) {
    if (sock_ == kInvalid) {
        err = "socket is not connected";
        return -1;
    }
    const int n = ::recv(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(buf), static_cast<int>(len), 0);
    if (n > 0) return n;
    if (n == 0) return 0;   // 对端关闭
    const int code = WSAGetLastError();
    if (code == WSAETIMEDOUT) {
        err = "receive timed out";
    } else if (code == WSAECONNRESET) {
        err = "connection reset by peer";
    } else {
        err = lastError("recv");
    }
    return -1;
}

} // namespace jc2::net
