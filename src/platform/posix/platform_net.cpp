/**
 * @file platform_net_posix.cpp
 * @brief POSIX/Linux implementation of platform_net.h
 * 
 * Uses standard POSIX socket API and getifaddrs() for multi-NIC support.
 */

#include "platform_net.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <ifaddrs.h>
#include <cstring>
#include <errno.h>

// ============================================================================
// Initialization/Cleanup
// ============================================================================

PlatformSocketError platform_net_init() {
    // POSIX sockets don't require initialization
    return PlatformSocketError::Success;
}

PlatformSocketError platform_net_cleanup() {
    // POSIX sockets don't require cleanup
    return PlatformSocketError::Success;
}

// ============================================================================
// TCP Socket Operations
// ============================================================================

PlatformSocket platform_socket() {
    int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return INVALID_PLATFORM_SOCKET;
    }
    return static_cast<PlatformSocket>(sock);
}

PlatformSocketError platform_close(PlatformSocket sock) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::SocketError;
    }
    int fd = static_cast<int>(sock);
    if (::close(fd) != 0) {
        return PlatformSocketError::SocketError;
    }
    return PlatformSocketError::Success;
}

PlatformSocketError platform_connect(PlatformSocket sock, const std::string& host,
                                    uint16_t port, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::NotInitialized;
    }

    int fd = static_cast<int>(sock);

    // Resolve hostname to IP address
    struct hostent* server = ::gethostbyname(host.c_str());
    if (!server) {
        return PlatformSocketError::ConnectError;
    }

    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    // Set non-blocking mode for timeout handling
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return PlatformSocketError::ConnectError;
    }
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Try to connect
    if (::connect(fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == 0) {
        // Connected immediately
        ::fcntl(fd, F_SETFL, flags);  // Restore blocking mode
        return PlatformSocketError::Success;
    }

    if (errno != EINPROGRESS) {
        ::fcntl(fd, F_SETFL, flags);  // Restore blocking mode
        return PlatformSocketError::ConnectError;
    }

    // Wait for connection with timeout
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int poll_result = ::poll(&pfd, 1, timeout_ms);

    ::fcntl(fd, F_SETFL, flags);  // Restore blocking mode

    if (poll_result <= 0) {
        return PlatformSocketError::TimeoutError;
    }

    // Check if connection succeeded
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0 || error != 0) {
        return PlatformSocketError::ConnectError;
    }

    return PlatformSocketError::Success;
}

int platform_send(PlatformSocket sock, const void* data, int len, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    int fd = static_cast<int>(sock);

    // Set send timeout
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int result = ::send(fd, data, len, 0);
    return result;
}

int platform_recv(PlatformSocket sock, void* buffer, int len, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    int fd = static_cast<int>(sock);

    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int result = ::recv(fd, buffer, len, 0);
    return result;
}

PlatformSocketError platform_setsockopt(PlatformSocket sock, const std::string& opt_name,
                                        const void* opt_value, int opt_len) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::OptionError;
    }

    int fd = static_cast<int>(sock);
    int so_level = SOL_SOCKET;
    int so_optname = 0;

    if (opt_name == "SO_RCVTIMEO") {
        so_optname = SO_RCVTIMEO;
    } else if (opt_name == "SO_SNDTIMEO") {
        so_optname = SO_SNDTIMEO;
    } else if (opt_name == "SO_REUSEADDR") {
        so_optname = SO_REUSEADDR;
    } else {
        return PlatformSocketError::OptionError;
    }

    if (::setsockopt(fd, so_level, so_optname, opt_value, opt_len) != 0) {
        return PlatformSocketError::OptionError;
    }

    return PlatformSocketError::Success;
}

// ============================================================================
// Multi-NIC Support: Get Local IP for Remote Address
// ============================================================================

PlatformSocketError platform_get_local_ip_for_remote(const std::string& remote_ip,
                                                     std::string& local_ip_out) {
    // Try to connect to remote IP to determine routing
    // This works by creating a socket and connecting, then querying the local address
    int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return PlatformSocketError::SocketError;
    }

    struct sockaddr_in remote_addr;
    std::memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(53);  // Use DNS port (doesn't need to be open)
    remote_addr.sin_addr.s_addr = inet_addr(remote_ip.c_str());

    // Connect (doesn't actually establish connection for UDP, just sets default destination)
    if (::connect(sock, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) != 0) {
        ::close(sock);
        return PlatformSocketError::ConnectError;
    }

    // Get the local address that would be used
    struct sockaddr_in local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    if (::getsockname(sock, (struct sockaddr*)&local_addr, &local_addr_len) != 0) {
        ::close(sock);
        return PlatformSocketError::SocketError;
    }

    ::close(sock);

    // Convert to string
    char addr_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &local_addr.sin_addr, addr_str, INET_ADDRSTRLEN) == nullptr) {
        return PlatformSocketError::SocketError;
    }

    local_ip_out = addr_str;
    return PlatformSocketError::Success;
}

// ============================================================================
// UDP Socket Operations
// ============================================================================

PlatformSocket platform_udp_socket() {
    int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return INVALID_PLATFORM_SOCKET;
    }
    return static_cast<PlatformSocket>(sock);
}

PlatformSocketError platform_bind(PlatformSocket sock, const std::string& local_ip, uint16_t port) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return PlatformSocketError::BindError;
    }

    int fd = static_cast<int>(sock);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(local_ip.c_str());

    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        return PlatformSocketError::BindError;
    }

    return PlatformSocketError::Success;
}

int platform_sendto(PlatformSocket sock, const std::string& remote_ip, uint16_t remote_port,
                   const void* data, int len) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    int fd = static_cast<int>(sock);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remote_port);
    addr.sin_addr.s_addr = inet_addr(remote_ip.c_str());

    int result = ::sendto(fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    return result;
}

PlatformSocketError platform_listen(PlatformSocket sock, int backlog) {
    if (sock == INVALID_PLATFORM_SOCKET) return PlatformSocketError::ListenError;
    int fd = static_cast<int>(sock);
    if (::listen(fd, backlog) != 0) return PlatformSocketError::ListenError;
    return PlatformSocketError::Success;
}

PlatformSocket platform_accept(PlatformSocket sock) {
    if (sock == INVALID_PLATFORM_SOCKET) return INVALID_PLATFORM_SOCKET;
    int fd     = static_cast<int>(sock);
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) return INVALID_PLATFORM_SOCKET;
    return static_cast<PlatformSocket>(client);
}

PlatformSocketError platform_set_recv_timeout(PlatformSocket sock, int timeout_ms) {
    if (sock == INVALID_PLATFORM_SOCKET) return PlatformSocketError::OptionError;
    int fd = static_cast<int>(sock);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return PlatformSocketError::OptionError;
    return PlatformSocketError::Success;
}

PlatformSocketError platform_set_reuseaddr(PlatformSocket sock) {
    if (sock == INVALID_PLATFORM_SOCKET) return PlatformSocketError::OptionError;
    int fd  = static_cast<int>(sock);
    int opt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0)
        return PlatformSocketError::OptionError;
    return PlatformSocketError::Success;
}

int platform_recvfrom(PlatformSocket sock, void* buffer, int len, int timeout_ms,
                      std::string& remote_ip_out, uint16_t& remote_port_out) {
    if (sock == INVALID_PLATFORM_SOCKET) {
        return -1;
    }

    int fd = static_cast<int>(sock);

    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int result = ::recvfrom(fd, buffer, len, 0, (struct sockaddr*)&addr, &addr_len);

    if (result > 0) {
        char addr_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr.sin_addr, addr_str, INET_ADDRSTRLEN) != nullptr) {
            remote_ip_out = addr_str;
            remote_port_out = ntohs(addr.sin_port);
        }
    }

    return result;
}
