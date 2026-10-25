#pragma once
#include <cstdint>

// Render-thread-only, one-slot retry mailbox. Owns no textures: the compositor
// retains the current A/B pair. New captures supersede unsubmitted requests.
class LatestFrameRequest {
public:
    void Clear() { id_ = 0; busy_ = false; denied_ = false; }
    bool Offer(int64_t id) {
        if (id == id_) return false;
        const bool replaced = id_ != 0;
        id_ = id; busy_ = false; denied_ = false;
        return replaced;
    }
    bool Pending(int64_t id) const { return id_ != 0 && id_ == id; }
    bool MarkBusy() { const bool first = !busy_; busy_ = true; return first; }
    bool MarkDenied() { const bool first = !denied_; denied_ = true; return first; }
    bool Retried() const { return busy_ || denied_; }
private:
    int64_t id_ = 0;
    bool busy_ = false, denied_ = false;
};