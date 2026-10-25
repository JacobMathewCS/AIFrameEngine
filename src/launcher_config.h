#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace launcher {
namespace fs = std::filesystem;

struct Options {
    bool fp16 = true;
    bool fsr1 = true;
    bool cache = true;
};

struct Target {
    HWND window = nullptr;
    DWORD pid = 0;
    unsigned long long created = 0;
    std::wstring title;
};

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct LaunchGuard {
    HANDLE value = nullptr;
    bool acquired = false;
    LaunchGuard() {
        value = CreateMutexW(nullptr, TRUE, L"Local\\AIFrameEngine.Launcher.Start");
        acquired = value && GetLastError() != ERROR_ALREADY_EXISTS;
    }
    ~LaunchGuard() {
        if (acquired) ReleaseMutex(value);
        if (value) CloseHandle(value);
    }
    LaunchGuard(const LaunchGuard&) = delete;
    LaunchGuard& operator=(const LaunchGuard&) = delete;
};

inline std::wstring ErrorText(DWORD code) {
    wchar_t text[1024] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, text, 1024, nullptr);
    return std::wstring(text) + L" (Windows error " + std::to_wstring(code) + L")";
}

inline bool Identify(HWND window, Target& target) {
    DWORD pid = 0;
    RECT client{};
    if (!IsWindowVisible(window) || IsIconic(window) ||
        GetAncestor(window, GA_ROOT) != window || !GetWindowThreadProcessId(window, &pid) ||
        !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process.value || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT ||
        !GetProcessTimes(process.value, &created, &exited, &kernel, &user)) return false;
    wchar_t title[512] = {};
    GetWindowTextW(window, title, 512);
    if (!*title) return false;
    target = {window, pid, (static_cast<unsigned long long>(created.dwHighDateTime) << 32) |
        created.dwLowDateTime, title};
    return true;
}

inline bool StillSameTarget(const Target& expected) {
    Target current;
    return Identify(expected.window, current) && current.pid == expected.pid &&
        current.created == expected.created;
}

// Fail closed if process enumeration fails; a second compositor registers the
// same global hotkeys and can capture the wrong session.
inline bool CompositorRunning(bool& running, std::wstring& error) {
    running = false;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) { error = ErrorText(GetLastError()); return false; }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) { error = ErrorText(GetLastError()); return false; }
    do {
        if (_wcsicmp(entry.szExeFile, L"AIFrameEngineCompositor.exe") == 0) {
            running = true; return true;
        }
    } while (Process32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) { error = ErrorText(GetLastError()); return false; }
    return true;
}

inline bool NonemptyFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && fs::file_size(path, error) > 0 && !error;
}

inline std::vector<fs::path> MissingFiles(const fs::path& executable, const Options& options) {
    const auto base = executable.parent_path();
    std::vector<fs::path> required = {executable, base / L"onnxruntime.dll", base / L"DirectML.dll",
        base / L"models/RIFE_flow_576x320.onnx"};
    if (options.fp16) required.push_back(base / L"models/RIFE_flow_576x320_fp16.onnx");
    if (options.fsr1) {
        for (auto name : {L"fsr1.hlsl", L"ffx_a.h", L"ffx_fsr1.h"})
            required.push_back(base / L"shaders" / name);
    }
    std::vector<fs::path> missing;
    for (const auto& file : required) if (!NonemptyFile(file)) missing.push_back(file);
    return missing;
}

struct EnvironmentLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
            b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
using Environment = std::map<std::wstring, std::wstring, EnvironmentLess>;

inline Environment ReadEnvironment(const wchar_t* block) {
    Environment result;
    if (!block) return result;
    for (auto item = block; *item; item += wcslen(item) + 1) {
        std::wstring entry(item);
        // Preserve drive-current-directory entries such as =C:=C:\work.
        auto separator = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
        if (separator != std::wstring::npos)
            result[entry.substr(0, separator)] = entry.substr(separator + 1);
    }
    return result;
}

inline std::vector<wchar_t> ChildEnvironment(Environment environment, const Target& target,
                                            const Options& options) {
    // GUI launches are predictable, not affected by old shell profiling/probe
    // settings. Only this child's environment changes, never the user's settings.
    for (auto it = environment.begin(); it != environment.end();) {
        if (it->first.size() >= 8 && _wcsnicmp(it->first.c_str(), L"AIFRAME_", 8) == 0)
            it = environment.erase(it);
        else ++it;
    }
    environment[L"AIFRAME_RIFE_FP16"] = options.fp16 ? L"1" : L"0";
    environment[L"AIFRAME_FSR1"] = options.fsr1 ? L"1" : L"0";
    environment[L"AIFRAME_DISPLAY_CACHE"] = options.cache ? L"1" : L"0";
    environment[L"AIFRAME_TARGET_HWND"] = std::to_wstring(reinterpret_cast<uintptr_t>(target.window));
    environment[L"AIFRAME_TARGET_PID"] = std::to_wstring(target.pid);
    environment[L"AIFRAME_TARGET_CREATED"] = std::to_wstring(target.created);
    std::vector<wchar_t> block;
    for (const auto& entry : environment) {
        const auto line = entry.first + L"=" + entry.second;
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

inline bool StartChild(const fs::path& executable, std::vector<wchar_t>& environment,
                       PROCESS_INFORMATION& process, std::wstring& error) {
    // Explicit application path and no shell parsing: spaces and Unicode remain
    // paths, never commands. No inherited handles or global environment edits.
    auto command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    process = {};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, environment.data(), executable.parent_path().c_str(),
        &startup, &process)) {
        error = ErrorText(GetLastError()); return false;
    }
    return true;
}
} // namespace launcher