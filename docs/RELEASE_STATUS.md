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