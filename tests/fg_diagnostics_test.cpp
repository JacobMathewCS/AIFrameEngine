#include "../src/fg_diagnostics.h"
#include "../src/frame_timeline.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    FgDiagnostics disabled;
    Check(!disabled.Enabled(), "diagnostics default off");
    disabled.Record({"worker", "completed", 7, 900, 100});
    std::ostringstream empty;
    Check(disabled.Write(empty, 42), "empty trace write");
    Check(empty.str().find("\nworker,completed,") == std::string::npos, "disabled does not record");
    const auto emptyText = empty.str();
    Check(std::count(emptyText.begin(), emptyText.end(), '\n') == 2,
        "disabled trace contains only header and summary");

    FgDiagnostics trace;
    trace.Enable(2);
    FgDiagnostics::Row row;
    row.event = "worker"; row.reason = "completed"; row.pair = 7;
    row.now = 900; row.capture = 100; row.submitted = 200;
    row.workerStart = 300; row.completed = 900;
    trace.Record(row);
    row.event = "attach"; row.reason = "expired"; row.now = 1000;
    trace.Record(row);
    trace.Record(row);
    std::ostringstream csv;
    Check(trace.Write(csv, 42), "trace serialization");
    Check(csv.str().find("worker,completed,42,7,900,100,0,200,300,900,") != std::string::npos,
        "pair keyed worker timestamps preserved");
    Check(csv.str().find("attach,expired,42,7,1000,") != std::string::npos, "reason preserved");
    Check(csv.str().substr(csv.str().size() - 3) == ",1\n", "overflow explicitly counted");
    std::istringstream lines(csv.str());
    std::string line;
    int lineCount = 0;
    while (std::getline(lines, line)) {
        Check(std::count(line.begin(), line.end(), ',') == 20, "all CSV rows have 21 columns");
        ++lineCount;
    }
    Check(lineCount == 4, "header plus bounded rows plus summary");
    std::ostringstream failed;
    failed.setstate(std::ios::badbit);
    Check(!trace.Write(failed, 42), "failed output detected");

    FgDiagnostics presentation;
    presentation.Enable(3);
    row = {};
    row.event = "attach"; row.reason = "attached"; row.pair = 7; row.now = 100;
    presentation.Record(row);
    row.event = "select"; row.reason = "generated_selected";
    row.now = 110; row.selectionAt = 110; row.content = 50;
    row.candidatePair = 7; row.generated = row.candidateGenerated = row.enabled = true;
    presentation.Record(row);
    row.event = "present"; row.reason = "not_submitted_ok";
    row.now = 130; row.presentStart = 120; row.hresult = -1;
    presentation.Record(row);
    std::ostringstream outcomes;
    Check(presentation.Write(outcomes, 42), "presentation serialization");
    Check(outcomes.str().find("select,generated_selected,42,7,110,0,0,0,0,0,0,50,7,1,1,1,0,0,110,0,0\n")
        != std::string::npos, "selection identity is independent of presentation");
    Check(outcomes.str().find("present,not_submitted_ok,42,7,130,0,0,0,0,0,0,50,7,1,1,1,0,-1,110,120,0\n")
        != std::string::npos, "failed Present preserves selected identity and signed HRESULT");

    FgDiagnostics concurrent;
    concurrent.Enable(500);
    auto record = [&concurrent] {
        for (int i = 0; i < 400; ++i) concurrent.Record({"worker", "completed", i});
    };
    std::thread a(record), b(record);
    a.join(); b.join();
    std::ostringstream joined;
    Check(concurrent.Write(joined, 42), "concurrent record serialization");
    const auto text = joined.str();
    Check(std::count(text.begin(), text.end(), '\n') == 502, "concurrent capacity enforced");
    Check(text.substr(text.size() - 5) == ",300\n", "concurrent drops counted");

    FrameTimeline<int> timeline;
    PairAvailability reason = PairAvailability::Available;
    int sourceA = -1, sourceB = -1;
    Check(!timeline.Sources(7, 0, sourceA, sourceB, &reason) && reason == PairAvailability::Missing,
        "missing source reason");
    timeline.Add(7, 100, 200, 1, 2);
    const int64_t deadline = 200 + timeline.GetDelay();
    Check(timeline.Sources(7, deadline - 1, sourceA, sourceB, &reason) &&
        reason == PairAvailability::Available && sourceA == 1 && sourceB == 2, "valid source reason");
    Check(timeline.Attach(7, 3, deadline - 1, &reason) && reason == PairAvailability::Available,
        "valid attach reason");
    Check(!timeline.Sources(7, deadline, sourceA, sourceB, &reason) && reason == PairAvailability::Expired,
        "source expires exactly at boundary");
    Check(!timeline.Attach(7, 4, deadline, &reason) && reason == PairAvailability::Expired,
        "attach expires exactly at boundary");
    timeline.Clear();
    Check(!timeline.Attach(7, 4, 0, &reason) && reason == PairAvailability::Missing,
        "cleared pair reports missing not expired");
    std::cout << "PASS: bounded optional FG diagnostics, concurrent writes, CSV schema, timeline reasons\n";
}