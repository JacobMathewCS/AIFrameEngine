#pragma once
#include "frame_timeline.h"

// Render-thread only. Commit only after successful Present; failed or occluded
// presents must not advance the visible-content clock. Used during FG as well
// as startup so clearing/refilling the timeline cannot replay older content.
template<class Frame>
class PresentationGuard {
public:
    using Selection = typename FrameTimeline<Frame>::Selection;
    Selection Choose(const Selection& candidate) const {
        if (last_.frame && (!candidate.frame || candidate.contentTime < last_.contentTime))
            return last_;
        return candidate;
    }
    void Commit(const Selection& shown) { if (shown.frame) last_ = shown; }
    void Clear() { last_ = {}; }
private:
    Selection last_{};
};