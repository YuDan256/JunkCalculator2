#include "../jc2_extension_cpp.h"
#include <string>
#include <vector>
#include <cmath>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET NativeSocket;
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
typedef int NativeSocket;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

static void initNetwork() {
    static bool initialized = false;
    if (!initialized) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            jc2::throw_error("Network Error: WSAStartup failed.");
        }
#endif
        initialized = true;
    }
}

struct SocketWrapper {
    NativeSocket sock;
    SocketWrapper(NativeSocket s) : sock(s) {}
    ~SocketWrapper() { if (sock != INVALID_SOCKET) closesocket(sock); }
};

static jc2::Class* g_socketClass = nullptr;

static SocketWrapper* getSock(const jc2::Value& v, const std::string& fn) {
    if (!v.is_instance()) jc2::throw_error("Type Error: " + fn + " expects a valid Network Socket.");
    auto ptr = v.get_native_data<SocketWrapper>();
    if (!ptr) jc2::throw_error("Type Error: " + fn + " expects a valid Network Socket.");
    return ptr;
}

static jc2::Value makeSocketInstance(NativeSocket s) {
    jc2::Instance inst(*g_socketClass);
    auto ptr = new SocketWrapper(s);
    inst.set_native_data(ptr, [](void* p) {
        delete static_cast<SocketWrapper*>(p);
    });
    return inst;
}

#define METHOD(name) JC2_ValueHandle sock_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)
#define GET_SELF auto wrapper = getSock(jc2::Value(argv[0]), #name)

METHOD(send) {
    (void)argc;
    GET_SELF;
    std::string data = jc2::Value(argv[1]).as_string();
    if (::send(wrapper->sock, data.c_str(), (int)data.size(), 0) == SOCKET_ERROR) {
        jc2::throw_error("Network Error: Connection lost during send.");
    }
    return jc2::Value(static_cast<double>(data.size())).get_handle();
}

METHOD(recv) {
    GET_SELF;
    int max_bytes = argc > 1 ? static_cast<int>(jc2::Value(argv[1]).as_double()) : 4096;
    if (max_bytes <= 0) max_bytes = 4096;

    std::vector<char> buffer(max_bytes);
    int bytes_read = ::recv(wrapper->sock, buffer.data(), max_bytes, 0);

    if (bytes_read < 0) jc2::throw_error("Network Error: Failed to receive data.");
    if (bytes_read == 0) return jc2::Value("").get_handle();

    return jc2::Value(std::string(buffer.data(), bytes_read)).get_handle();
}

METHOD(close) {
    (void)argc;
    GET_SELF;
    if (wrapper->sock != INVALID_SOCKET) {
        closesocket(wrapper->sock);
        wrapper->sock = INVALID_SOCKET;
    }
    return jc2::Value().get_handle();
}

METHOD(accept) {
    (void)argc;
    GET_SELF;
    NativeSocket client_sock = ::accept(wrapper->sock, nullptr, nullptr);
    if (client_sock == INVALID_SOCKET) {
        jc2::throw_error("Network Error: Accept failed.");
    }
    return makeSocketInstance(client_sock).get_handle();
}

#define FUNC(name) JC2_ValueHandle global_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)

FUNC(connect) {
    (void)argc;
    std::string host = jc2::Value(argv[0]).as_string();
    std::string port = std::to_string(static_cast<int>(std::round(jc2::Value(argv[1]).as_double())));

    struct addrinfo hints = { 0 }, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        jc2::throw_error("Network Error: Could not resolve host '" + host + "'.");
    }

    NativeSocket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        jc2::throw_error("Network Error: Failed to create socket.");
    }

    if (::connect(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        closesocket(s);
        freeaddrinfo(res);
        jc2::throw_error("Network Error: Connection refused to " + host + ":" + port);
    }
    freeaddrinfo(res);

    return makeSocketInstance(s).get_handle();
}

FUNC(server) {
    (void)argc;
    std::string host = jc2::Value(argv[0]).as_string();
    std::string port = std::to_string(static_cast<int>(std::round(jc2::Value(argv[1]).as_double())));

    struct addrinfo hints = { 0 }, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const char* host_ptr = (host == "0.0.0.0" || host == "") ? nullptr : host.c_str();

    if (getaddrinfo(host_ptr, port.c_str(), &hints, &res) != 0) {
        jc2::throw_error("Network Error: Could not resolve bind address.");
    }

    NativeSocket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        jc2::throw_error("Network Error: Failed to create server socket.");
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (::bind(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        closesocket(s);
        freeaddrinfo(res);
        jc2::throw_error("Network Error: Bind failed on port " + port);
    }
    freeaddrinfo(res);

    if (::listen(s, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(s);
        jc2::throw_error("Network Error: Listen failed.");
    }

    return makeSocketInstance(s).get_handle();
}

int jc2_init(jc2::Module& mod) {
    initNetwork();

    g_socketClass = new jc2::Class("Socket");
    mod.register_value("Socket", *g_socketClass);

    g_socketClass->bind_method("send", sock_send, 1, 1, false);
    g_socketClass->bind_method("recv", sock_recv, 0, 1, false);
    g_socketClass->bind_method("close", sock_close, 0, 0, false);
    g_socketClass->bind_method("accept", sock_accept, 0, 0, false);

    mod.register_function("connect", global_connect, 2, 2, false);
    mod.register_function("server", global_server, 2, 2, false);

    mod.register_help("socket",
        "═══ Native Socket Binding — Native Module ═══\n\n"
        "  Requires: import socket\n\n"
        "  The `socket` module provides unfiltered access to the operating system's \n"
        "  network stack (WinSock2 on Windows, POSIX Sockets on Linux/macOS).\n\n"
        "  ★ Note: For everyday networking, it is HIGHLY recommended to use the standard \n"
        "    library wrapper `import net`, which abstracts these pointers into managed \n"
        "    TCP objects (`TcpSocket` and `TcpServer`).\n\n"
        "  Outbound Connections (Client)\n"
        "  ──────────────────────\n"
        "    socket.connect(host, port)\n"
        "        Resolves domain records (DNS) and dials a TCP stream to the remote peer.\n"
        "        Returns an opaque pointer wrapped inside a `Socket` Class instance.\n\n"
        "  Inbound Listeners (Server)\n"
        "  ──────────────────────\n"
        "    socket.server(host, port)\n"
        "        Binds to a specified local interface (e.g., \"127.0.0.1\" or \"0.0.0.0\") \n"
        "        and puts the OS networking stack into LISTEN mode with SO_REUSEADDR enabled.\n"
        "        Returns a server socket wrapper.\n\n"
        "    server.accept()\n"
        "        Blocks the active VM thread pending incoming network traffic. \n"
        "        When a peer connects, returns a brand-new client socket wrapper.\n\n"
        "  Raw Data Exchange / Teardown\n"
        "  ──────────────────────\n"
        "    sock.send(text)\n"
        "        Performs a deep C++ 'send()' call, flushing strings over the TCP boundary.\n"
        "    sock.recv(max_bytes)\n"
        "        Blocks and reads incoming packets. Returns \"\" on disconnect.\n"
        "    sock.close()\n"
        "        Silently severs an established network pipe."
    );

    mod.register_function_help("socket.connect", "socket.connect(host, port)", "Dials a TCP stream to the remote peer.", "socket.connect(\"127.0.0.1\", 8080)");
    mod.register_function_help("socket.send", "sock.send(text)", "Sends a string over the TCP socket.", "sock.send(\"Hello\")");
    mod.register_function_help("socket.recv", "sock.recv(max_bytes)", "Blocks and reads incoming packets from the socket.", "sock.recv(1024)");
    mod.register_function_help("socket.close", "sock.close()", "Closes the network socket.", "sock.close()");
    mod.register_function_help("socket.server", "socket.server(host, port)", "Binds to a local interface and listens for incoming TCP connections.", "socket.server(\"0.0.0.0\", 8080)");
    mod.register_function_help("socket.accept", "server.accept()", "Blocks and accepts an incoming connection, returning a client socket.", "server.accept()");

    return 0;
}

JC2_EXTENSION_INIT
