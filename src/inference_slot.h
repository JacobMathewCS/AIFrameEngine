#pragma once

// Caller holds queueMutex_ and resultMutex_. Publication precedes worker idle.
inline bool CanSubmitInference(bool busy, bool resultReady) {
    return !busy && !resultReady;
}