/**
 * @file platform_net_windows.cpp
 * @brief Windows implementation of platform_net.h
 * 
 * Uses Winsock2 API for Windows networking.
 */

#include "platform_net.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wchar.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winmm.lib")

// ============================================================================
// Initialization/Cleanup
// ============================================================================

static bool g_wsa_initialized = false;

PlatformSocketError platform_net_init() {
    if (g_wsa_initialized) {
        return PlatformSocketError::Success;
    }

    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return PlatformSocketError::NotInitialized;
    }

    g_wsa_initialized = true;
    return PlatformSocketError::Success;
}

PlatformSocketError platform_net_cleanup() {
    if (!g_wsa_initialized) {
        return PlatformSocketError::Success;
    }

    WSACleanup();
    g_wsa_initialized = false;
    return PlatformSocketError::Success;
}

// ============================================================================
// TCP Socket Operations
// ============================================================================

PlatformSocket platform_socket() {
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return INVALID_PLATFORM_SOCKET;
    }
    return static_cast<PlatformSocket>(sock);
}

PlatformSocketError platform_close(PlatformSocket sock) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::SocketError;
    }
    SOCKET ws_sock = static_cast<SOCKET>(sock);
    if (::closesocket(ws_sock) != 0) {
        return PlatformSocketError::SocketError;
    }
    return PlatformSocketError::Success;
}

PlatformSocketError platform_connect(PlatformSocket sock, const std::string& host,
                                    uint16_t port, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::NotInitialized;
    }

    SOCKET ws_sock = static_cast<SOCKET>(sock);

    // Resolve hostname to IP address
    struct hostent* server = ::gethostbyname(host.c_str());
    if (!server) {
        return PlatformSocketError::ConnectError;
    }

    struct sockaddr_in serv_addr;
    ZeroMemory(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    CopyMemory(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    // Set timeout
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    ::setsockopt(ws_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    ::setsockopt(ws_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    // Try to connect
    if (::connect(ws_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) != 0) {
        return PlatformSocketError::ConnectError;
    }

    return PlatformSocketError::Success;
}

int platform_send(PlatformSocket sock, const void* data, int len, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    SOCKET ws_sock = static_cast<SOCKET>(sock);

    // Set send timeout
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    ::setsockopt(ws_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    int result = ::send(ws_sock, (const char*)data, len, 0);
    return result;
}

int platform_recv(PlatformSocket sock, void* buffer, int len, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    SOCKET ws_sock = static_cast<SOCKET>(sock);

    // Set receive timeout
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    ::setsockopt(ws_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    int result = ::recv(ws_sock, (char*)buffer, len, 0);
    return result;
}

PlatformSocketError platform_setsockopt(PlatformSocket sock, const std::string& opt_name,
                                        const void* opt_value, int opt_len) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::OptionError;
    }

    SOCKET ws_sock = static_cast<SOCKET>(sock);
    int so_level = SOL_SOCKET;
    int so_optname = 0;

    if (opt_name == "SO_RCVTIMEO") {
        so_optname = SO_RCVTIMEO;
    } else if (opt_name == "SO_SNDTIMEO") {
        so_optname = SO_SNDTIMEO;
    } else if (opt_name == "SO_REUSEADDR") {
        so_optname = SO_REUSEADDR;
    } else if (opt_name == "SO_BROADCAST") {
        so_optname = SO_BROADCAST;
    } else {
        return PlatformSocketError::OptionError;
    }

    if (::setsockopt(ws_sock, so_level, so_optname, (const char*)opt_value, opt_len) != 0) {
        return PlatformSocketError::OptionError;
    }

    return PlatformSocketError::Success;
}

// ============================================================================
// Multi-NIC Support: Get Local IP for Remote Address
// ============================================================================

PlatformSocketError platform_get_local_ip_for_remote(const std::string& remote_ip,
                                                     std::string& local_ip_out) {
    // Use WSAIoctl with SIO_ROUTING_INTERFACE_QUERY
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return PlatformSocketError::SocketError;
    }

    struct sockaddr_in remote_addr;
    ZeroMemory(&remote_addr, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(1);
    remote_addr.sin_addr.s_addr = inet_addr(remote_ip.c_str());

    struct sockaddr_in local_addr;
    ZeroMemory(&local_addr, sizeof(local_addr));
    DWORD bytes_returned = 0;

    if (WSAIoctl(sock, SIO_ROUTING_INTERFACE_QUERY, &remote_addr, sizeof(remote_addr),
                 &local_addr, sizeof(local_addr), &bytes_returned, NULL, NULL) != 0) {
        ::closesocket(sock);
        return PlatformSocketError::ConnectError;
    }

    ::closesocket(sock);

    // Convert to string
    char addr_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &local_addr.sin_addr, addr_str, INET_ADDRSTRLEN) == NULL) {
        return PlatformSocketError::SocketError;
    }

    local_ip_out = addr_str;
    return PlatformSocketError::Success;
}

// ============================================================================
// UDP Socket Operations
// ============================================================================

PlatformSocket platform_udp_socket() {
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_PLATFORM_SOCKET;
    }
    return static_cast<PlatformSocket>(sock);
}

PlatformSocketError platform_bind(PlatformSocket sock, const std::string& local_ip, uint16_t port) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::BindError;
    }

    SOCKET ws_sock = static_cast<SOCKET>(sock);

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(local_ip.c_str());

    if (::bind(ws_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        return PlatformSocketError::BindError;
    }

    return PlatformSocketError::Success;
}

int platform_sendto(PlatformSocket sock, const std::string& remote_ip, uint16_t remote_port,
                   const void* data, int len) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    SOCKET ws_sock = static_cast<SOCKET>(sock);

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remote_port);
    addr.sin_addr.s_addr = inet_addr(remote_ip.c_str());

    int result = ::sendto(ws_sock, (const char*)data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    return result;
}

PlatformSocketError platform_listen(PlatformSocket sock, int backlog) {
    if (sock == INVALID_PLATFORM_SOCKET) return PlatformSocketError::ListenError;
    SOCKET ws = static_cast<SOCKET>(sock);
    if (::listen(ws, backlog) != 0) return PlatformSocketError::ListenError;
    return PlatformSocketError::Success;
}

PlatformSocket platform_accept(PlatformSocket sock) {
    if (sock == INVALID_PLATFORM_SOCKET) return INVALID_PLATFORM_SOCKET;
    SOCKET ws     = static_cast<SOCKET>(sock);
    SOCKET client = ::accept(ws, nullptr, nullptr);
    if (client == INVALID_SOCKET) return INVALID_PLATFORM_SOCKET;
    return static_cast<PlatformSocket>(client);
}

PlatformSocketError platform_set_recv_timeout(PlatformSocket sock, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) return PlatformSocketError::OptionError;
    SOCKET ws = static_cast<SOCKET>(sock);
    DWORD ms  = static_cast<DWORD>(timeout_ms);
    if (::setsockopt(ws, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0)
        return PlatformSocketError::OptionError;
    return PlatformSocketError::Success;
}

PlatformSocketError platform_set_reuseaddr(PlatformSocket sock) {
    if (sock == INVALID_PLATFORM_SOCKET) return PlatformSocketError::OptionError;
    SOCKET ws = static_cast<SOCKET>(sock);
    int opt   = 1;
    if (::setsockopt(ws, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0)
        return PlatformSocketError::OptionError;
    return PlatformSocketError::Success;
}

int platform_recvfrom(PlatformSocket sock, void* buffer, int len, int timeout_ms,
                      std::string& remote_ip_out, uint16_t& remote_port_out) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    SOCKET ws_sock = static_cast<SOCKET>(sock);

    // Set receive timeout
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    ::setsockopt(ws_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    int result = ::recvfrom(ws_sock, (char*)buffer, len, 0, (struct sockaddr*)&addr, &addr_len);

    if (result > 0) {
        char addr_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr.sin_addr, addr_str, INET_ADDRSTRLEN) != NULL) {
            remote_ip_out = addr_str;
            remote_port_out = ntohs(addr.sin_port);
        }
    }

    return result;
}
