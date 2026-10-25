#pragma once
#include <cstdint>

// Diagnostic dose, not an optimization. Fail closed for malformed environment
// values; cap at four EXTRA copies in addition to the normal cache restore.
inline unsigned ParseRepeatCopyProbe(const char* value) {
    return value && value[0] >= '0' && value[0] <= '4' && value[1] == '\0'
        ? unsigned(value[0] - '0') : 0;
}

// Render-thread only. Production Frame is a ComPtr, retaining the previous SRV
// so allocator reuse cannot masquerade as a repeated immutable image.
template<class Frame>
class RepeatCopyProbe {
public:
    struct Key {
        Frame frame{};
        int64_t content = 0;
        uint64_t revision = 0;
        bool generated = false, fg = false;
        int mode = 0;
    };
    void Configure(unsigned extra) {
        extra_ = extra <= 4 ? extra : 0;
        previous_ = {}; valid_ = false;
    }
    unsigned Extra() const { return extra_; }
    unsigned Copies(bool cacheHit, const Key& key) const {
        return extra_ && cacheHit && valid_ && key.frame &&
            key.frame == previous_.frame && key.content == previous_.content &&
            key.revision == previous_.revision && key.generated == previous_.generated &&
            key.fg == previous_.fg && key.mode == previous_.mode ? extra_ : 0;
    }
    void Commit(bool submittedOk, bool immutableCacheable, const Key& key) {
        valid_ = extra_ && submittedOk && immutableCacheable && key.frame;
        previous_ = valid_ ? key : Key{};
    }
private:
    unsigned extra_ = 0;
    bool valid_ = false;
    Key previous_{};
};