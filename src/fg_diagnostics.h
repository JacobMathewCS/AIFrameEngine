#pragma once
#include <cstdint>
#include <mutex>
#include <ostream>
#include <vector>

// Optional diagnostic only. Enable before starting workers; write after joining.
// Event/reason strings must be static literals. No file IO in Record().
class FgDiagnostics {
public:
    struct Row {
        const char* event = "";
        const char* reason = "";
        int64_t pair = 0, now = 0, capture = 0, serviceStart = 0;
        int64_t submitted = 0, workerStart = 0, completed = 0, delay = 0;
        int64_t content = 0, candidatePair = 0, selectionAt = 0, presentStart = 0;
        bool generated = false, candidateGenerated = false, enabled = false;
        int mode = 0;
        int64_t hresult = 0;
    };
    void Enable(size_t limit = 30000) {
        rows_.reserve(limit);
        limit_ = limit;
    }
    bool Enabled() const { return limit_ != 0; }
    void Record(const Row& row) {
        if (!Enabled()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (rows_.size() < limit_) rows_.push_back(row);
        else ++dropped_;
    }
    bool Write(std::ostream& out, uint32_t pid) const {
        // Caller has joined workers; buffer insertion order is not time order.
        out << "event,reason,pid,pair,now_100ns,capture_100ns,service_start_100ns,submitted_100ns,worker_start_100ns,completed_100ns,delay_100ns,content_100ns,candidate_pair,generated,candidate_generated,fg_enabled,display_mode,hresult,selection_100ns,present_start_100ns,dropped_rows\n";
        for (const auto& r : rows_) {
            out << r.event << ',' << r.reason << ',' << pid << ',' << r.pair << ','
                << r.now << ',' << r.capture << ',' << r.serviceStart << ','
                << r.submitted << ',' << r.workerStart << ',' << r.completed << ','
                << r.delay << ',' << r.content << ',' << r.candidatePair << ','
                << r.generated << ',' << r.candidateGenerated << ',' << r.enabled << ','
                << r.mode << ',' << r.hresult << ',' << r.selectionAt << ',' << r.presentStart << ",0\n";
        }
        out << "end,buffer_summary," << pid << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0," << dropped_ << '\n';
        out.flush();
        return static_cast<bool>(out);
    }
private:
    size_t limit_ = 0;
    uint64_t dropped_ = 0;
    std::vector<Row> rows_;
    std::mutex mutex_;
};