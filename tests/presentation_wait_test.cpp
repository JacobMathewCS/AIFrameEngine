#include "../src/presentation_wait.h"
#include <iostream>
#include <stdexcept>

static void Check(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
int main() {
    HANDLE completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE display = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int status = 0;
    try {
        Check(completion && display, "event creation");
        MSG msg = {}; // Ensure this thread owns a message queue.
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        Check(WaitForPresentation(completion, display, 0) == PresentationWake::Poll, "bounded idle wait");
        SetEvent(completion);
        Check(WaitForPresentation(completion, display, 0) == PresentationWake::Completion, "worker wakes without display permission");
        Check(WaitForPresentation(completion, display, 0) == PresentationWake::Poll, "completion event is consumed");
        SetEvent(completion); SetEvent(display);
        Check(WaitForPresentation(completion, display, 0) == PresentationWake::Completion, "consume result before simultaneous display signal");
        Check(WaitForPresentation(completion, display, 0) == PresentationWake::Display, "display permission is not lost");
        Check(PostThreadMessageW(GetCurrentThreadId(), WM_APP, 0, 0) != 0, "post message");
        Check(WaitForPresentation(completion, display, 0) == PresentationWake::Messages, "messages remain responsive");
        PeekMessageW(&msg, nullptr, WM_APP, WM_APP, PM_REMOVE);
        Check(WaitForPresentation(nullptr, display, 0) == PresentationWake::Failed, "invalid handle fails instead of spinning");
        std::cout << "PASS: bounded wait, completion priority, display permission, messages, failure\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; status=1; }
    if (completion) CloseHandle(completion);
    if (display) CloseHandle(display);
    return status;
}