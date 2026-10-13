# Contributing

Please discuss substantial changes before replacing a working model or rendering path.
Keep experimental behavior opt-in and preserve a measured baseline.

1. Provision dependencies using `external/README.md` and `models/README.md`.
2. Build and run CTest in Release and Debug on Windows x64.
3. Run `python -m unittest discover -s tests -p "test_*.py"` from the checkout root
   for the summary-tool regressions.
4. Add deterministic regression coverage for scheduler, crop, control or identity changes.
5. For GPU changes, report hardware, resolution, model precision, test duration, frame
   pacing and artifacts. Distinguish submissions from displayed events and physical scanout.

Never include model weights, SDK binaries, credentials, raw gameplay captures, profiler
traces or personal paths in a pull request. Redact window titles and usernames from logs.
Do not describe a benchmark as a speedup without a comparable baseline.

Project-wide licensing remains an owner decision. Resolve it before accepting substantial
external contributions or distributing binaries. Dependency/model terms require separate review.