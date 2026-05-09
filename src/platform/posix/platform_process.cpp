/**
 * @file platform_process_posix.cpp
 * @brief POSIX/Linux implementation of platform_process.h
 * 
 * Uses fork/exec for process spawning and pipe(2) for inter-process communication.
 */

#include "platform_process.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <poll.h>
#include <signal.h>
#include <cstring>
#include <map>
#include <mutex>

/**
 * @brief Global map to track running processes
 * Used to maintain process state and handle non-blocking waits
 */
static std::map<pid_t, bool> g_running_processes;
static std::mutex g_process_mutex;

// ============================================================================
// Pipe Operations
// ============================================================================

PlatformProcessError platform_create_pipe(PlatformPipe& read_end, PlatformPipe& write_end) {
    int pipes[2];
    if (::pipe(pipes) != 0) {
        return PlatformProcessError::PipeError;
    }
    read_end = static_cast<PlatformPipe>(pipes[0]);
    write_end = static_cast<PlatformPipe>(pipes[1]);
    return PlatformProcessError::Success;
}

PlatformProcessError platform_close_pipe(PlatformPipe pipe) {
    if (pipe == INVALID_PIPE) {
        return PlatformProcessError::PipeError;
    }
    int fd = static_cast<int>(pipe);
    if (::close(fd) != 0) {
        return PlatformProcessError::PipeError;
    }
    return PlatformProcessError::Success;
}

int platform_read_pipe(PlatformPipe pipe, void* buffer, int buffer_len) {
    if (pipe == INVALID_PIPE || !buffer || buffer_len <= 0) {
        return -1;
    }
    int fd = static_cast<int>(pipe);
    ssize_t result = ::read(fd, buffer, buffer_len);
    return static_cast<int>(result);
}

int platform_write_pipe(PlatformPipe pipe, const void* data, int data_len) {
    if (pipe == INVALID_PIPE || !data || data_len <= 0) {
        return -1;
    }
    int fd = static_cast<int>(pipe);
    ssize_t result = ::write(fd, data, data_len);
    return static_cast<int>(result);
}

// ============================================================================
// Process Management
// ============================================================================

PlatformProcess platform_spawn_process(const std::string& executable,
                                      const std::vector<std::string>& args,
                                      PlatformPipe& stdin_pipe,
                                      PlatformPipe& stdout_pipe,
                                      PlatformPipe& stderr_pipe) {
    if (executable.empty()) {
        return INVALID_PROCESS;
    }

    int stdin_pipes[2]  = {-1, -1};
    int stdout_pipes[2] = {-1, -1};
    int stderr_pipes[2] = {-1, -1};

    auto close_all = [&]() {
        for (int fd : stdin_pipes)  if (fd >= 0) ::close(fd);
        for (int fd : stdout_pipes) if (fd >= 0) ::close(fd);
        for (int fd : stderr_pipes) if (fd >= 0) ::close(fd);
    };

    if (::pipe(stdin_pipes)  != 0 ||
        ::pipe(stdout_pipes) != 0 ||
        ::pipe(stderr_pipes) != 0) {
        close_all();
        return INVALID_PROCESS;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        close_all();
        return INVALID_PROCESS;
    }

    if (pid == 0) {
        // Child: wire our pipe ends to stdin/stdout/stderr
        ::dup2(stdin_pipes[0],  STDIN_FILENO);
        ::dup2(stdout_pipes[1], STDOUT_FILENO);
        ::dup2(stderr_pipes[1], STDERR_FILENO);

        // Close all pipe fds (dup2 duplicated them)
        for (int fd : stdin_pipes)  ::close(fd);
        for (int fd : stdout_pipes) ::close(fd);
        for (int fd : stderr_pipes) ::close(fd);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        ::execvp(executable.c_str(), argv.data());
        ::_exit(127);  // execvp failed
    }

    // Parent: close child-side ends
    ::close(stdin_pipes[0]);
    ::close(stdout_pipes[1]);
    ::close(stderr_pipes[1]);

    stdin_pipe  = static_cast<PlatformPipe>(stdin_pipes[1]);
    stdout_pipe = static_cast<PlatformPipe>(stdout_pipes[0]);
    stderr_pipe = static_cast<PlatformPipe>(stderr_pipes[0]);

    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_running_processes[pid] = true;
    }

    return static_cast<PlatformProcess>(pid);
}

PlatformProcessError platform_wait_process(PlatformProcess proc, int& exit_code, int timeout_ms) {
    if (proc == INVALID_PROCESS) {
        return PlatformProcessError::InvalidHandle;
    }

    pid_t pid = static_cast<pid_t>(proc);
    int status = 0;
    pid_t result;

    if (timeout_ms < 0) {
        // Infinite wait
        result = ::waitpid(pid, &status, 0);
    } else {
        // With timeout: use non-blocking wait and poll
        struct pollfd pfd;
        pfd.fd = -1;  // poll doesn't work with processes directly
        pfd.events = 0;

        // For now, implement timeout using waitpid with WNOHANG in a loop
        int elapsed = 0;
        const int poll_interval = 10;  // milliseconds

        while (elapsed < timeout_ms) {
            result = ::waitpid(pid, &status, WNOHANG);
            if (result != 0) {
                break;
            }
            ::usleep(poll_interval * 1000);
            elapsed += poll_interval;
        }

        if (result == 0) {
            // Timeout
            return PlatformProcessError::WaitError;
        }
    }

    if (result < 0) {
        return PlatformProcessError::WaitError;
    }

    // Remove from tracking map
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_running_processes.erase(pid);
    }

    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = -WTERMSIG(status);
    } else {
        exit_code = -1;
    }

    return PlatformProcessError::Success;
}

PlatformProcessError platform_kill_process(PlatformProcess proc) {
    if (proc == INVALID_PROCESS) {
        return PlatformProcessError::InvalidHandle;
    }

    pid_t pid = static_cast<pid_t>(proc);

    ::kill(pid, SIGKILL);

    // Blocking wait — SIGKILL cannot be ignored so the process exits immediately
    int status = 0;
    ::waitpid(pid, &status, 0);

    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_running_processes.erase(pid);
    }

    return PlatformProcessError::Success;
}

bool platform_process_is_running(PlatformProcess proc) {
    if (proc == INVALID_PROCESS) {
        return false;
    }

    pid_t pid = static_cast<pid_t>(proc);

    // Check if process is still in our tracking map
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        auto it = g_running_processes.find(pid);
        if (it == g_running_processes.end()) {
            return false;
        }
    }

    // Try a non-blocking waitpid to see if process has exited
    int status = 0;
    pid_t result = ::waitpid(pid, &status, WNOHANG);

    if (result == pid) {
        // Process has exited
        {
            std::lock_guard<std::mutex> lock(g_process_mutex);
            g_running_processes.erase(pid);
        }
        return false;
    }

    return true;
}
