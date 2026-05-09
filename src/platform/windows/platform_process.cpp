/**
 * @file platform_process_windows.cpp
 * @brief Windows implementation of platform_process.h
 * 
 * Uses Windows CreateProcess API for process spawning and CreatePipe for IPC.
 */

#include "platform_process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wchar.h>
#include <map>
#include <mutex>

/**
 * @brief Global map to track running process handles
 */
static std::map<intptr_t, HANDLE> g_process_handles;
static std::mutex g_process_mutex;

// Helper to convert std::string to std::wstring
static std::wstring string_to_wstring(const std::string& str) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size_needed);
    return wstr;
}

// ============================================================================
// Pipe Operations
// ============================================================================

PlatformProcessError platform_create_pipe(PlatformPipe& read_end, PlatformPipe& write_end) {
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        return PlatformProcessError::PipeError;
    }

    read_end  = reinterpret_cast<PlatformPipe>(hRead);
    write_end = reinterpret_cast<PlatformPipe>(hWrite);
    return PlatformProcessError::Success;
}

PlatformProcessError platform_close_pipe(PlatformPipe pipe) {
    if (pipe == INVALID_PIPE) {
        return PlatformProcessError::PipeError;
    }
    HANDLE h = reinterpret_cast<HANDLE>(pipe);
    if (!CloseHandle(h)) {
        return PlatformProcessError::PipeError;
    }
    return PlatformProcessError::Success;
}

int platform_read_pipe(PlatformPipe pipe, void* buffer, int buffer_len) {
    if (pipe == INVALID_PIPE || !buffer || buffer_len <= 0) {
        return -1;
    }
    HANDLE h = reinterpret_cast<HANDLE>(pipe);
    DWORD bytes_read = 0;
    if (!ReadFile(h, buffer, buffer_len, &bytes_read, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
            return 0;  // EOF: write end closed (process exited)
        return -1;
    }
    return static_cast<int>(bytes_read);
}

int platform_write_pipe(PlatformPipe pipe, const void* data, int data_len) {
    if (pipe == INVALID_PIPE || !data || data_len <= 0) {
        return -1;
    }
    HANDLE h = reinterpret_cast<HANDLE>(pipe);
    DWORD bytes_written = 0;
    if (!WriteFile(h, data, data_len, &bytes_written, NULL)) {
        return -1;
    }
    return static_cast<int>(bytes_written);
}

// ============================================================================
// Process Management
// ============================================================================

// Quote an arg if it contains spaces or special characters
static std::string quote_arg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos)
        return arg;
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else          out += c;
    }
    out += '"';
    return out;
}

PlatformProcess platform_spawn_process(const std::string& executable,
                                      const std::vector<std::string>& args,
                                      PlatformPipe& stdin_pipe,
                                      PlatformPipe& stdout_pipe,
                                      PlatformPipe& stderr_pipe) {
    if (executable.empty()) {
        return INVALID_PROCESS;
    }

    HANDLE hStdoutRead, hStdoutWrite;
    HANDLE hStderrRead, hStderrWrite;
    HANDLE hStdinRead,  hStdinWrite;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hStdinRead,   &hStdinWrite,  &sa, 0))    return INVALID_PROCESS;
    if (!CreatePipe(&hStdoutRead,  &hStdoutWrite, &sa, 65536)) { CloseHandle(hStdinRead); CloseHandle(hStdinWrite); return INVALID_PROCESS; }
    if (!CreatePipe(&hStderrRead,  &hStderrWrite, &sa, 4096)) {
        CloseHandle(hStdinRead);  CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        return INVALID_PROCESS;
    }

    // Parent-side handles must NOT be inherited by the child
    SetHandleInformation(hStdinWrite,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutRead,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStderrRead,  HANDLE_FLAG_INHERIT, 0);

    // Build quoted command line
    std::string cmd = quote_arg(executable);
    for (const auto& arg : args) {
        cmd += " ";
        cmd += quote_arg(arg);
    }
    std::wstring wcmd = string_to_wstring(cmd);

    // Use PROC_THREAD_ATTRIBUTE_HANDLE_LIST so only the three child-side pipe
    // handles are inherited.  Without this, every inheritable handle in the
    // parent process (including pipes from other concurrently-spawned ffmpeg
    // instances) would be inherited, keeping extra write-ends alive and causing
    // ReadFile on stderr to never return EOF after the child exits.
    HANDLE inheritHandles[3] = { hStdinRead, hStdoutWrite, hStderrWrite };

    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList =
        static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attrListSize));
    if (!attrList ||
        !InitializeProcThreadAttributeList(attrList, 1, 0, &attrListSize) ||
        !UpdateProcThreadAttribute(attrList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritHandles, sizeof(inheritHandles), nullptr, nullptr))
    {
        if (attrList) HeapFree(GetProcessHeap(), 0, attrList);
        CloseHandle(hStdinRead);  CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        CloseHandle(hStderrRead); CloseHandle(hStderrWrite);
        return INVALID_PROCESS;
    }

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb        = sizeof(STARTUPINFOEXW);
    si.StartupInfo.hStdInput  = hStdinRead;
    si.StartupInfo.hStdOutput = hStdoutWrite;
    si.StartupInfo.hStdError  = hStderrWrite;
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.lpAttributeList        = attrList;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessW(NULL, (LPWSTR)wcmd.c_str(), NULL, NULL,
                             TRUE,
                             CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             NULL, NULL,
                             reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);

    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);

    // Close child-side handles in parent regardless of outcome
    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    if (!ok) {
        CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        return INVALID_PROCESS;
    }

    CloseHandle(pi.hThread);

    stdin_pipe  = reinterpret_cast<PlatformPipe>(hStdinWrite);
    stdout_pipe = reinterpret_cast<PlatformPipe>(hStdoutRead);
    stderr_pipe = reinterpret_cast<PlatformPipe>(hStderrRead);

    intptr_t proc_id = reinterpret_cast<intptr_t>(pi.hProcess);
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_process_handles[proc_id] = pi.hProcess;
    }

    return proc_id;
}

PlatformProcessError platform_wait_process(PlatformProcess proc, int& exit_code, int timeout_ms) {
    if (proc == INVALID_PROCESS) {
        return PlatformProcessError::InvalidHandle;
    }

    HANDLE hProcess = reinterpret_cast<HANDLE>(proc);
    DWORD timeout = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);

    DWORD result = WaitForSingleObject(hProcess, timeout);
    if (result == WAIT_TIMEOUT) {
        return PlatformProcessError::WaitError;
    }

    if (result != WAIT_OBJECT_0) {
        return PlatformProcessError::WaitError;
    }

    // Get exit code
    DWORD dwExitCode = 0;
    if (!GetExitCodeProcess(hProcess, &dwExitCode)) {
        return PlatformProcessError::WaitError;
    }

    exit_code = static_cast<int>(dwExitCode);

    // Clean up handle
    CloseHandle(hProcess);

    // Remove from tracking map
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        intptr_t proc_id = static_cast<intptr_t>(proc);
        g_process_handles.erase(proc_id);
    }

    return PlatformProcessError::Success;
}

PlatformProcessError platform_kill_process(PlatformProcess proc) {
    if (proc == INVALID_PROCESS) {
        return PlatformProcessError::InvalidHandle;
    }

    HANDLE hProcess = reinterpret_cast<HANDLE>(proc);

    if (!TerminateProcess(hProcess, 1)) {
        return PlatformProcessError::KillError;
    }

    // Wait briefly for process to terminate
    WaitForSingleObject(hProcess, 1000);

    // Clean up handle
    CloseHandle(hProcess);

    // Remove from tracking map
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        intptr_t proc_id = static_cast<intptr_t>(proc);
        g_process_handles.erase(proc_id);
    }

    return PlatformProcessError::Success;
}

bool platform_process_is_running(PlatformProcess proc) {
    if (proc == INVALID_PROCESS) {
        return false;
    }

    HANDLE hProcess = reinterpret_cast<HANDLE>(proc);

    // Check if process has exited
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(hProcess, &exit_code)) {
        return false;
    }

    // If exit code is still STILL_ACTIVE, process is running
    if (exit_code == STILL_ACTIVE) {
        return true;
    }

    // Process has exited, clean up
    CloseHandle(hProcess);

    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        intptr_t proc_id = static_cast<intptr_t>(proc);
        g_process_handles.erase(proc_id);
    }

    return false;
}
