# GPU-resident frame-generation experiment

Standalone correctness prototype, **not integrated into the compositor**. The root
build does not enable or deploy this code. Original project code is covered by the
repository MIT license; SDKs and model weights are provisioned separately.

The path snapshots a D3D11 frame pair, resizes and packs model inputs on the GPU,
runs DirectML with resident input/output buffers, checks field finiteness, and
reconstructs into a shared D3D11 output texture. Inference fields are 576 x 320;
reconstruction can retain the original capture resolution.

No pixel or flow-field data is transferred through CPU memory inside the candidate
path. This is **not literally copy-free**: GPU-local snapshot, compact-image and
buffer-to-texture copies remain, as does a four-byte finite-status CPU readback.
The normalizer uploads a constant lookup table, not frame pixels.

## Build the isolated library

Use Windows x64, MSVC, a recent Windows SDK, and CMake 3.25 or newer. Provision
matching ONNX Runtime DirectML headers, import library and runtime DLLs using the
repository's dependency instructions. No dependency downloads happen here.

From the repository root in PowerShell:

```powershell
$root = (Get-Location).Path
cmake -S "$root\experiments\gpu_resident" -B "$root\build-gpu-resident" -A x64
cmake --build "$root\build-gpu-resident" --config Release
cmake --build "$root\build-gpu-resident" --config Debug
```

Set `-DAIFRAME_ORT_ROOT=<absolute SDK directory>` if the matching `include/` and
`lib/` files live outside the checkout. No weights or SDK binaries are included.

## Ownership and integration limits

- Single worker, single-use jobs and exclusive D3D11 immediate-context access.
- Both preprocessing and reconstruction use producer-owned snapshots of the same
  pair. Caller captures must remain ready and immutable until producer completion.
- Dependencies retain command resources, session, inputs and outputs through
  retirement. A timeout is not cancellation. Do not destroy a pending job.
- NaNs/infinities reject publication. The status wait is bounded but blocking;
  per-job allocations and waiting are correctness scaffolding, not a speed claim.
- Device-loss/shutdown recovery, asynchronous renderer/worker handoff, capture
  leases, pooling, stale-generation/deadline rejection and production selection
  are unfinished. Do not use this helper directly in the compositor worker.

`interop/baseline/` contains frozen first-party resize, packing and reconstruction
references so tests do not silently change with the production renderer. It is
not a bundled third-party SDK. Keep reference changes separate from candidate
changes when interpreting parity results.

## Safety checks (no model inference)

The standalone build includes a test executable unless `BUILD_TESTING=OFF`.
CTest runs WARP synthetic finite/publication checks and rejects a missing mode.
Hardware safety is separately opt-in and intentionally restricted to the tested
NVIDIA device ID `10de:2520`; other GPUs fail that gate rather than silently falling
back. The library itself does not contain that test-only adapter restriction.

```powershell
ctest --test-dir "$root\build-gpu-resident" -C Release --output-on-failure
ctest --test-dir "$root\build-gpu-resident" -C Debug --output-on-failure
python -B -m unittest discover -s "$root\experiments\gpu_resident" -p 'test_*.py' -v
```

Python 3.11 or newer is required for the standard-library-only artifact auditor.
Enable hardware safety with `-DAIFRAME_GPU_HARDWARE_SAFETY=ON` when configuring a
fresh build. Each safety run exercises 80 finite cases and two publication cases,
including original-pair snapshot ownership, without creating a model session.
CTest timeouts bound the process; they do not prove safe in-process cancellation.

## Explicit model gate

Model inference is never part of CTest. The executable accepts the explicit mode
`--execute-fp16-combined`, an absolute model filename, and its independently
verified lowercase SHA-256. It rejects a missing/invalid hash before GPU creation,
loads at most 64 MiB, and uses the exact hashed byte snapshot for session creation.
Only use a trusted, self-contained flow/mask export: `img0` and `img1` must be FP32
RGB NCHW `[1,3,320,576]`; `flow_mask` must be FP32 `[1,5,320,576]`.
The FP16 designation refers to internal model arithmetic, not its external tensors.
The generic hash argument verifies identity, **not internal FP16 conversion**;
establish that separately for any supplied model. No weights are distributed here.

Run only after reviewing the model/runtime provenance and choosing a fresh, empty
artifact directory outside the checkout. Bound the process externally and record
exit status and timeout. The gate attempts three fixture pairs, with at most six
inference calls total (CPU-I/O baseline plus resident candidate per pair). There is
no automatic retry or tolerance relaxation. Preprocessing is bit-exact; field
error is `abs(actual-reference)/(1+abs(reference)) <= 1e-4`; reconstructed pixels
allow at most one code value; the shared draw must be byte-exact. Native fixture
output is 1920 x 1080. Test-only CPU readbacks are not part of the candidate path.

`audit_combined.py` takes an artifact directory and a new JSON output filename.
It checks all 18 saved field/pixel files without executing native code or inference.
It detects changes during auditing and records relative artifact hashes. This is
a numerical audit, **not** verification of execution provenance, preprocessing,
runtime identity or the six-run limit. Preserve the execution log and independently
pin the executable, model, runtime and source when collecting evidence. Neither
raw evidence nor local machine paths belong in a commit.

## Evidence boundaries

The preceding scratch collection passed Release/Debug safety checks and a bounded
three-pair FP16 hardware run. Its independent saved-artifact audit reported zero
field/pixel differences and exact draw handoff for all three pairs. That evidence
is unsealed and applies to the scratch snapshot, not automatically to this portable
build. The portable harness changes model pinning from a local generated header
to an explicit caller-supplied hash. Model inference must not be rerun merely to
audit existing evidence. No speedup, gameplay quality, sustained cadence, broad
hardware portability or production readiness is claimed.