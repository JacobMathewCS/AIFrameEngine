# Contributing

Please discuss substantial changes before replacing a working model or rendering path.
Keep experimental behavior opt-in and preserve a measured baseline.

1. Provision dependencies using `external/README.md` and `models/README.md`.
2. Build and run CTest in Release and Debug on Windows x64.
3. The public source release includes native tests. Local diagnostic Python suites
   are not part of this release; do not infer their coverage from launcher CI.
4. Add deterministic regression coverage for scheduler, crop, control or identity changes.
5. For GPU changes, report hardware, resolution, model precision, test duration, frame
   pacing and artifacts. Distinguish submissions from displayed events and physical scanout.

Never include model weights, SDK binaries, credentials, raw gameplay captures, profiler
traces or personal paths in a pull request. Redact window titles and usernames from logs.
Do not describe a benchmark as a speedup without a comparable baseline.

Original project code is MIT licensed; see LICENSE. Dependency/model terms require
separate review, and must be retained when redistributing those components.