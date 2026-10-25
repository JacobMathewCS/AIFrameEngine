#pragma once
#include <windows.h>
#include <cstdint>
#include <limits>
#include <string>

inline bool ParseTargetNumber(const std::wstring& text, uint64_t& value) {
    value = 0;
    if (text.empty()) return false;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        const auto digit = uint64_t(c - L'0');
        if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}

// One capture target per UI thread. Pin the process lifetime and observe window
// destruction so HWND/PID reuse does not become automatic target reselection.
class CaptureTarget {
public:
    CaptureTarget() = default;
    CaptureTarget(const CaptureTarget&) = delete;
    CaptureTarget& operator=(const CaptureTarget&) = delete;
    ~CaptureTarget() { Reset(); }
    bool Bound() const { return window_ != nullptr; }
    HWND Window() const { return window_; }
    DWORD ProcessId() const { return pid_; }
    bool Bind(HWND window, DWORD expectedPid = 0, uint64_t expectedCreation = 0) {
        Reset();
        DWORD pid = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &pid);
        RECT client = {};
        if (!thread || !pid || (expectedPid && pid != expectedPid) ||
            !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window ||
            !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return false;
        process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process_) return false;
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process_, &creation, &exit, &kernel, &user) ||
            (expectedCreation && expectedCreation != (uint64_t(creation.dwHighDateTime) << 32 | creation.dwLowDateTime))) {
            Reset(); return false;
        }
        window_ = window; pid_ = pid; thread_ = thread;
        active_ = this;
        hook_ = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr,
            Destroyed, pid_, thread_, WINEVENT_OUTOFCONTEXT);
        if (!hook_ || !Valid()) { Reset(); return false; }
        return true;
    }
    bool Valid() const {
        DWORD pid = 0;
        return window_ && !destroyed_ && process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT &&
            GetWindowThreadProcessId(window_, &pid) == thread_ && pid == pid_ && IsWindow(window_);
    }
    std::wstring Title() const {
        wchar_t title[512] = {};
        if (Valid()) GetWindowTextW(window_, title, 512);
        return *title ? std::wstring(title) : L"Selected window";
    }
private:
    static void CALLBACK Destroyed(HWINEVENTHOOK hook, DWORD, HWND window, LONG object,
                                   LONG child, DWORD, DWORD) {
        if (active_ && active_->hook_ == hook && active_->window_ == window &&
            object == OBJID_WINDOW && child == CHILDID_SELF) active_->destroyed_ = true;
    }
    void Reset() {
        if (hook_) UnhookWinEvent(hook_);
        if (active_ == this) active_ = nullptr;
        if (process_) CloseHandle(process_);
        hook_ = nullptr; process_ = nullptr; window_ = nullptr;
        pid_ = thread_ = 0; destroyed_ = false;
    }
    inline static thread_local CaptureTarget* active_ = nullptr;
    HWND window_ = nullptr;
    HANDLE process_ = nullptr;
    HWINEVENTHOOK hook_ = nullptr;
    DWORD pid_ = 0, thread_ = 0;
    bool destroyed_ = false;
};