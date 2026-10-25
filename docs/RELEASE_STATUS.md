# Release status

## Published scope

- Original C++ compositor, launcher, headers and project shader.
- Native regression tests and a synthetic ONNX I/O fixture.
- CMake integration, PowerShell launcher, research model exporters and setup notes.
- MIT license for original project work; upstream attribution and model references.
- Standalone-launcher CI. This is not full compositor/GPU certification.

The development checkout and deployed compositor are kept separate from this
publication checkout. Private gameplay, traces, historical handoffs, downloaded
SDKs and pretrained models are not published. Local diagnostic Python suites are
not included because their complete helper/evidence dependencies need a separate
review; the source release makes no claim to include those tests.

## Remaining release gates

- Identify and pin the exact ImGui revision and matching DirectML/runtime packages.
- Add integrity-checked dependency acquisition and validate a clean full build.
- Confirm checkpoint distribution terms; record model hashes and external types.
- Validate FP32 export and FP16 provisioning from documented, pinned inputs.
- Package runtime notices before distributing a full binary/model bundle.
- Finish manual keyboard, DPI/monitor, missing-file and relaunch coverage.

The maintainer has reported working GUI-launched gameplay, interpolation and
upscaling. Native 1080p frame generation remains a known limitation. No new FPS or
latency claims follow from that report.

## Event submission checks (not an eligibility certification)

The supplied Hacktoberfest x MOSS Hack Day Udupi rulebook specifies build time of
10:00 AM to 5:00 PM IST on October 25, 2026. It disallows bringing pre-event project
code/materials and open-sourcing one's earlier work just to use it at the event.
AIFrameEngine includes pre-event work. Restoring or publishing this repository
does not make it eligible; disclose that fact to the organizers and obtain their
decision before submitting. Do not alter history or represent prior work as
event-day implementation.

Other owner/organizer checks still required:

- Confirm a 2–4-person roster; each member must satisfy student/age, registration
  and in-person check-in requirements. Solo entries are not allowed.
- Complete every AI coding tool/model name and the parts it produced, in both the
  README and submission. Current disclosure is explicitly incomplete.
- Confirm model terms and applicable challenge eligibility with the organizers.
- Submit through OrganizerHQ before 5:00 PM IST using the registered account;
  repository publication alone is not a submission. Demo URL is optional.
- Do not select the Gemma challenge: this project has no Gemma/Gemini integration.
- Keep the repository public after the event. No other-event submission claim is
  made; the team must confirm that requirement.
- Observe the rulebook's stop-building deadline and limited demo-bug exception.

No OrganizerHQ submission or statement of eligibility has been made by this release.