// 最小阻塞式 TCP 客户端（TLS 的传输层）。
// 只做 TLS 需要的事：带超时的 connect、send、recv、close。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace jc2::net {

class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    // 解析主机名并连接；timeoutMs 同时用作连接与读写超时。
    bool connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err);

    bool send(const uint8_t* data, size_t len, std::string& err);

    // 返回读到的字节数；0 = 对端正常关闭；-1 = 错误或超时（err 给出原因）。
    ptrdiff_t recv(uint8_t* buf, size_t len, std::string& err);

    void close();
    bool connected() const;

private:
    uintptr_t sock_ = static_cast<uintptr_t>(~0ull);
    int timeoutMs_ = 30000;
};

} // namespace jc2::net
