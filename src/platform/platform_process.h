/**
 * @file platform_process.h
 * @brief Cross-platform process management abstraction layer
 * 
 * This header defines a platform-agnostic API for spawning subprocesses,
 * managing pipes, and handling process lifecycle.
 * Abstracts the differences between Windows CreateProcess and POSIX fork/exec.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Opaque process handle
 * Windows: HANDLE
 * POSIX: pid_t
 */
using PlatformProcess = intptr_t;

/**
 * @brief Opaque pipe handle (file descriptor)
 * Windows: HANDLE
 * POSIX: int file descriptor
 */
using PlatformPipe = intptr_t;

/**
 * @brief Invalid process constant
 */
constexpr PlatformProcess INVALID_PROCESS = -1;

/**
 * @brief Invalid pipe constant
 */
constexpr PlatformPipe INVALID_PIPE = -1;

/**
 * @brief Process status/error codes
 */
enum class PlatformProcessError {
    Success = 0,
    SpawnError = -1,
    PipeError = -2,
    WaitError = -3,
    KillError = -4,
    InvalidHandle = -5
};

/**
 * @brief Create a pipe for inter-process communication
 * 
 * Creates a unidirectional pipe with a read end and a write end.
 * 
 * @param read_end Output: pipe handle for reading
 * @param write_end Output: pipe handle for writing
 * @return PlatformProcessError::Success on success
 */
PlatformProcessError platform_create_pipe(PlatformPipe& read_end, PlatformPipe& write_end);

/**
 * @brief Close a pipe handle
 * 
 * @param pipe Pipe to close
 * @return PlatformProcessError::Success on success
 */
PlatformProcessError platform_close_pipe(PlatformPipe pipe);

/**
 * @brief Read data from a pipe
 * 
 * Blocking read. Will wait for data or EOF.
 * 
 * @param pipe Pipe to read from
 * @param buffer Output buffer for data
 * @param buffer_len Maximum bytes to read
 * @return Number of bytes read on success, 0 on EOF, negative on error
 */
int platform_read_pipe(PlatformPipe pipe, void* buffer, int buffer_len);

/**
 * @brief Write data to a pipe
 * 
 * @param pipe Pipe to write to
 * @param data Data to write
 * @param data_len Number of bytes to write
 * @return Number of bytes written on success, negative on error
 */
int platform_write_pipe(PlatformPipe pipe, const void* data, int data_len);

/**
 * @brief Spawn a subprocess with optional stdout/stderr redirection
 * 
 * Spawns a child process and optionally redirects its stdout and stderr to pipes.
 * 
 * **Windows Implementation**:
 * - Uses CreateProcessW with STARTUPINFOW
 * - Inherits handles if redirecting
 * 
 * **POSIX Implementation**:
 * - Uses fork() + execvp()
 * - Child process inherits parent's environment
 * - Uses dup2() for pipe redirection
 * 
 * @param executable Program path or name (e.g., "ffmpeg" or "/usr/bin/ffmpeg")
 * @param args Command-line arguments (args[0] should typically be program name)
 * @param stdin_pipe  Output: pipe handle for writing to child's stdin
 * @param stdout_pipe Output: pipe handle for reading child's stdout
 * @param stderr_pipe Output: pipe handle for reading child's stderr
 * @return Valid PlatformProcess on success, INVALID_PROCESS on error
 */
PlatformProcess platform_spawn_process(const std::string& executable,
                                      const std::vector<std::string>& args,
                                      PlatformPipe& stdin_pipe,
                                      PlatformPipe& stdout_pipe,
                                      PlatformPipe& stderr_pipe);

/**
 * @brief Wait for a process to terminate
 * 
 * Blocks until the process terminates.
 * 
 * @param proc Process to wait for
 * @param exit_code Output: process exit code
 * @param timeout_ms Timeout in milliseconds (-1 for infinite)
 * @return PlatformProcessError::Success on success
 */
PlatformProcessError platform_wait_process(PlatformProcess proc, int& exit_code, int timeout_ms = -1);

/**
 * @brief Terminate a process
 * 
 * Forcefully kills a running process.
 * 
 * **Windows**: Uses TerminateProcess()
 * **POSIX**: Uses kill(SIGKILL)
 * 
 * @param proc Process to terminate
 * @return PlatformProcessError::Success on success
 */
PlatformProcessError platform_kill_process(PlatformProcess proc);

/**
 * @brief Check if a process is still running
 * 
 * Non-blocking check of process status.
 * 
 * @param proc Process to check
 * @return true if process is still running, false if terminated
 */
bool platform_process_is_running(PlatformProcess proc);
