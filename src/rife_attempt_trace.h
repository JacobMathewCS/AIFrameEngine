#pragma once
#include <cstdint>
#include <ostream>
#include <vector>

// Single inference-worker writer. Enable before workers, serialize after join.
// Static string literals only; bounded/preallocated; no file IO on the worker.
class RifeAttemptTrace {
public:
    struct Row {
        uint64_t attempt = 0, run = 0;
        int64_t pair = 0, start = 0, dispatchStart = 0, dispatchEnd = 0, end = 0;
        uint32_t tid = 0;
        const char* io = "none";
        const char* outcome = "unwound";
    };
    void Enable(size_t limit = 30000) { rows_.reserve(limit); limit_ = limit; }
    bool Enabled() const { return limit_ != 0; }
    bool Write(std::ostream& out, uint32_t pid) const {
        out << "event,version,pid,tid,attempt,pair,run,io,outcome,start_100ns,dispatch_start_100ns,dispatch_end_100ns,end_100ns,dropped_rows\n";
        for (const auto& r : rows_) {
            out << "attempt,1," << pid << ',' << r.tid << ',' << r.attempt << ',' << r.pair
                << ',' << r.run << ',' << r.io << ',' << r.outcome << ',' << r.start
                << ',' << r.dispatchStart << ',' << r.dispatchEnd << ',' << r.end << ",0\n";
        }
        out << "end,1," << pid << ",0,0,0,0,none,buffer_summary,0,0,0,0," << dropped_ << '\n';
        out.flush();
        return static_cast<bool>(out);
    }
    class Scope {
    public:
        Scope(RifeAttemptTrace& owner, int64_t pair, const char* io,
              int64_t (*clock)(), uint32_t (*threadId)()) : clock_(clock) {
            if (!owner.Enabled()) return;
            owner_ = &owner;
            row_.attempt = ++owner.next_;
            row_.pair = pair; row_.io = io;
            row_.tid = threadId(); row_.start = clock_();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() noexcept {
            if (!owner_) return;
            row_.end = clock_();
            if (owner_->rows_.size() < owner_->limit_) owner_->rows_.push_back(row_);
            else ++owner_->dropped_;
        }
        void Run(uint64_t id) { if (owner_) row_.run = id; }
        void DispatchStart(int64_t at) { if (owner_) row_.dispatchStart = at; }
        void DispatchEnd(int64_t at) { if (owner_) row_.dispatchEnd = at; }
        void Outcome(const char* value) { if (owner_) row_.outcome = value; }
    private:
        RifeAttemptTrace* owner_ = nullptr;
        int64_t (*clock_)();
        Row row_;
    };
private:
    size_t limit_ = 0;
    uint64_t next_ = 0, dropped_ = 0;
    std::vector<Row> rows_;
};