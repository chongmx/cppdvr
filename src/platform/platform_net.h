/**
 * @file platform_net.h
 * @brief Cross-platform networking abstraction layer
 * 
 * This header defines a platform-agnostic networking API that abstracts
 * the differences between Winsock2 (Windows) and POSIX sockets (Linux).
 */

#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Platform-agnostic socket handle
 * Windows: SOCKET (which is unsigned long long)
 * POSIX: int file descriptor
 */
using PlatformSocket = intptr_t;

/**
 * @brief Invalid socket constant
 */
constexpr PlatformSocket INVALID_PLATFORM_SOCKET = -1;

/**
 * @brief Platform socket error codes
 */
enum class PlatformSocketError {
    Success = 0,
    SocketError = -1,
    ConnectError = -2,
    TimeoutError = -3,
    BindError = -4,
    ListenError = -5,
    AcceptError = -6,
    SendError = -7,
    RecvError = -8,
    OptionError = -9,
    NotInitialized = -10
};

/**
 * @brief Initialize platform networking subsystem
 * 
 * Must be called before any socket operations.
 * Windows: Calls WSAStartup()
 * POSIX: No-op
 * 
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_net_init();

/**
 * @brief Cleanup platform networking subsystem
 * 
 * Should be called at application shutdown.
 * Windows: Calls WSACleanup()
 * POSIX: No-op
 * 
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_net_cleanup();

/**
 * @brief Create a TCP socket
 * 
 * @return Valid PlatformSocket on success, INVALID_PLATFORM_SOCKET on error
 */
PlatformSocket platform_socket();

/**
 * @brief Close a socket
 * 
 * @param sock Socket to close
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_close(PlatformSocket sock);

/**
 * @brief Connect to a remote TCP endpoint
 * 
 * @param sock Socket created with platform_socket()
 * @param host IP address or hostname
 * @param port Port number
 * @param timeout_ms Timeout in milliseconds
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_connect(PlatformSocket sock, const std::string& host, 
                                    uint16_t port, int timeout_ms);

/**
 * @brief Send data on a connected socket
 * 
 * @param sock Socket to send on
 * @param data Pointer to data to send
 * @param len Number of bytes to send
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes sent on success, negative on error
 */
int platform_send(PlatformSocket sock, const void* data, int len, int timeout_ms);

/**
 * @brief Receive data from a connected socket
 * 
 * @param sock Socket to receive from
 * @param buffer Pointer to receive buffer
 * @param len Maximum bytes to receive
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received on success, 0 on connection closed, negative on error
 */
int platform_recv(PlatformSocket sock, void* buffer, int len, int timeout_ms);

/**
 * @brief Set a socket option
 * 
 * Currently supports:
 * - SO_RCVTIMEO: Receive timeout (int milliseconds)
 * - SO_SNDTIMEO: Send timeout (int milliseconds)
 * 
 * @param sock Socket to configure
 * @param opt_name Option name (e.g., "SO_RCVTIMEO")
 * @param opt_value Pointer to option value
 * @param opt_len Length of option value
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_setsockopt(PlatformSocket sock, const std::string& opt_name,
                                        const void* opt_value, int opt_len);

/**
 * @brief Get the local IP address that routes to a remote IP
 * 
 * This is used to determine which local network interface to bind to when
 * serving to a specific remote peer. Useful on multi-homed systems.
 * 
 * Windows: Uses WSAIoctl(SIO_ROUTING_INTERFACE_QUERY)
 * POSIX: Loops through getifaddrs() and checks routing table
 * 
 * @param remote_ip Remote IP address (e.g., "192.168.1.1")
 * @param local_ip_out Output: local IP address that routes to remote_ip
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_get_local_ip_for_remote(const std::string& remote_ip,
                                                     std::string& local_ip_out);

/**
 * @brief Create a UDP socket
 * 
 * @return Valid PlatformSocket on success, INVALID_PLATFORM_SOCKET on error
 */
PlatformSocket platform_udp_socket();

/**
 * @brief Bind a socket to a local address
 * 
 * @param sock Socket to bind
 * @param local_ip Local IP address to bind to (e.g., "0.0.0.0" for any)
 * @param port Local port to bind to
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_bind(PlatformSocket sock, const std::string& local_ip, uint16_t port);

/**
 * @brief Send UDP data to a remote endpoint
 * 
 * @param sock UDP socket
 * @param remote_ip Remote IP address
 * @param remote_port Remote port
 * @param data Pointer to data to send
 * @param len Number of bytes to send
 * @return Number of bytes sent on success, negative on error
 */
int platform_sendto(PlatformSocket sock, const std::string& remote_ip, uint16_t remote_port,
                   const void* data, int len);

/**
 * @brief Receive UDP data from a remote endpoint
 * 
 * @param sock UDP socket
 * @param buffer Pointer to receive buffer
 * @param len Maximum bytes to receive
 * @param timeout_ms Timeout in milliseconds
 * @param remote_ip_out Output: source IP address
 * @param remote_port_out Output: source port
 * @return Number of bytes received on success, negative on error
 */
int platform_recvfrom(PlatformSocket sock, void* buffer, int len, int timeout_ms,
                      std::string& remote_ip_out, uint16_t& remote_port_out);

/**
 * @brief Put a socket into listening state for incoming TCP connections
 *
 * @param sock Socket bound with platform_bind()
 * @param backlog Maximum pending connection queue length
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_listen(PlatformSocket sock, int backlog);

/**
 * @brief Accept an incoming TCP connection
 *
 * Blocks until a client connects or the socket's receive timeout fires.
 *
 * @param sock Listening socket
 * @return New PlatformSocket for the client, INVALID_PLATFORM_SOCKET on timeout/error
 */
PlatformSocket platform_accept(PlatformSocket sock);

/**
 * @brief Set SO_RCVTIMEO on a socket (milliseconds, 0 = no timeout)
 *
 * Converts milliseconds to the platform-native timeout type.
 * Windows uses DWORD ms; POSIX uses struct timeval.
 *
 * @param sock Socket to configure
 * @param timeout_ms Timeout in milliseconds
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_set_recv_timeout(PlatformSocket sock, int timeout_ms);

/**
 * @brief Set SO_REUSEADDR on a socket
 *
 * Allows quick restart without "address already in use" errors.
 *
 * @param sock Socket to configure
 * @return PlatformSocketError::Success on success
 */
PlatformSocketError platform_set_reuseaddr(PlatformSocket sock);
