#include "../src/capture_target.h"
#include <iostream>
#include <stdexcept>

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        uint64_t number = 0;
        Require(ParseTargetNumber(L"123", number) && number == 123, "parse identity");
        for (auto text : {L"", L"0", L"-1", L"12x", L"18446744073709551616"})
            Require(!ParseTargetNumber(text, number), "reject invalid identity");
        HWND window = CreateWindowExW(0, L"STATIC", L"Capture target fixture",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 160, 100, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "create fixture window");
        CaptureTarget target;
        Require(!target.Bind(window, GetCurrentProcessId() + 1), "reject wrong PID");
        Require(!target.Bind(window, GetCurrentProcessId(), 1), "reject stale process lifetime");
        Require(target.Bind(window, GetCurrentProcessId()), "bind target");
        Require(target.Valid() && target.Title() == L"Capture target fixture", "target identity/title");
        DestroyWindow(window);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Require(!target.Valid() && target.Bound(), "closed target stays pinned but invalid");
        std::cout << "PASS: target parsing, identity, and closure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}