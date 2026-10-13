#pragma once
#include <windows.h>

enum class PresentationWake { Completion, Display, Messages, Poll, Failed };

// No immediate-context calls on a worker thread. A completion wakes the owner
// to consume the result; only DXGI's signal grants permission to draw/present.
inline PresentationWake WaitForPresentation(HANDLE completion, HANDLE display, DWORD timeoutMs = 2) {
    HANDLE handles[] = { completion, display };
    const DWORD result = MsgWaitForMultipleObjectsEx(2, handles, timeoutMs,
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    switch (result) {
    case WAIT_OBJECT_0: return PresentationWake::Completion;
    case WAIT_OBJECT_0 + 1: return PresentationWake::Display;
    case WAIT_OBJECT_0 + 2: return PresentationWake::Messages;
    case WAIT_TIMEOUT: return PresentationWake::Poll;
    default: return PresentationWake::Failed;
    }
}