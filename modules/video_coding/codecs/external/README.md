# External video encoders: NVENC, Intel QSV and x265

The vendor boundary is `VideoEncoderBackend` in `video_encoder_backend.h`.
Calls run serially on the WebRTC encoder sequence; there are no vendor callbacks.
`Frame` borrows CPU NV12 planes only for the duration of `Encode`.
Successful encoding returns one owned Annex B access unit with the input frame ID.
`Open`, `SetRates`, `Encode`, idempotent `Close`, `GetInfo`, and `LastError` are
the complete first-stage interface. Resolution/profile changes reopen the backend.

`BackendVideoEncoder` handles I420 conversion, RTP/capture metadata, SDP level
checks, keyframe requests, pause/resume, and WebRTC error/fallback translation.
The internal factory uses NVENC or QSV for H.264 Baseline, packetization-mode=1,
and one spatial/temporal layer. Constrained Baseline, Main, mode=0, and layered
configurations continue through the existing software path. A hardware failure
uses that encoder's existing OpenH264 software fallback wrapper. Automatic
creation tries NVENC, then QSV, then OpenH264, according to the compiled backends
and actual device probes. A later runtime failure goes to OpenH264; it does not
switch from one hardware vendor to the other.

An `Environment` can explicitly select a vendor with its `FieldTrials`:

```cpp
auto env = CreateEnvironment(std::make_unique<FieldTrials>(
    "WebRTC-H264-HardwareEncoder/QSV/"));
auto encoder = CreateBuiltinVideoEncoderFactory()->Create(env, format);
```

The exact group `NVENC` restricts creation to NVENC, and `QSV` restricts it to QSV.
For either explicit choice, an unavailable backend leads to OpenH264. Other or
absent groups use automatic creation order. Hardware smoke tests inject their
vendor explicitly and require the actual hardware implementation before/after
encoding, so another vendor or software cannot satisfy hardware acceptance.
`QueryCodecSupport` has no `Environment` argument: its `is_power_efficient`
reports whether either compiled backend can support the format, not whether an
environment's explicit preference will select hardware. Inspect the created
encoder's `GetEncoderInfo()` for the selected implementation.

H.265 supports Main profile, Main tier, 8-bit NV12, one spatial/temporal layer,
and SRST with no DON interleaving. Its negotiated level is passed to the vendor
backend. SPS profile/tier/level, cropped dimensions, chroma and bit depth are
checked, and requested/resumed keyframes require real IDR NALs with VPS/SPS/PPS.
The builtin factory advertises H.265 only after a real encoder session probe.
The opt-in x265 backend supplies CPU software HEVC encoding. Automatic H.265
creation tries NVENC, then QSV, then x265, according to the compiled backends
and actual probes. This is creation-time selection: a runtime failure in an
already selected encoder reports `WEBRTC_VIDEO_CODEC_ENCODER_FAILURE`; it does
not automatically change to another H.265 backend. The existing WebRTC
codec-switch callback, when installed, can act on this failure; automatic
recovery is not guaranteed.
The independent `WebRTC-H265-HardwareEncoder` field trial uses the exact group
`QSV` or `NVENC` to restrict H.265 creation to that vendor. Those explicit
hardware groups cannot fall back to x265 or the other hardware vendor.
`WebRTC-H265-Encoder/x265/` explicitly selects the x265 software encoder before
the hardware preference is considered. Without that software selection or an
explicit hardware group, creation uses the automatic order above. Inspect
`GetEncoderInfo()` to distinguish `x265 H265` with
`is_hardware_accelerated=false` from a hardware implementation.

Consumers enter this selection path through `CreateBuiltinVideoEncoderFactory()`.
A caller that builds its own `VideoEncoderFactoryTemplate` must explicitly adopt
the new factory/backend. With either hardware backend enabled, `webrtc.lib` includes
the selecting factory and backend; downstream Windows link commands need
`d3d11.lib` and `dxgi.lib` in addition to their existing WebRTC system libraries.
With QSV enabled, downstream consumers must also explicitly link the
CRT-compatible static `vpl.lib` and `advapi32.lib`, together with `d3d11.lib` and
`dxgi.lib`. An external GN `libs` input is not guaranteed to be incorporated in
the complete static `webrtc.lib`; downstream linking must carry this dependency.
With x265 enabled, downstream Windows consumers must explicitly link the
matching static `x265-static.lib` as well. Its external GN `libs` entry does not
guarantee that the library's objects are embedded in `webrtc.lib`.

The independently implemented backend uses the C NvEncodeAPI with D3D11.
It loads the NVIDIA driver DLL from System32. Only the SDK header is needed at
build time; neither OBS nor CUDA nor the NvCodec C++ wrappers are dependencies.
The current code uses SDK 12.2 fields and checks the driver's supported API.

QSV uses D3D11 on an Intel adapter and a static oneVPL dispatcher, with no OBS
runtime dependency. The current external oneVPL headers are API 2.14.0, commit
`025d43d086a3e663184cb49febe86152bf05409f`, under the MIT license. The dispatcher
can bridge to an Intel driver's legacy Media SDK runtime; headers at 2.14 do not
mean that the installed GPU runtime must implement API 2.14. Hardware session,
API/implementation and codec capability checks decide availability. An enabled
Intel graphics device and a compatible Intel driver are needed on the test host.
Driver DLLs are not bundled. QSV H.265 reuses the same synchronous backend and
WebRTC adapter boundary; HEVC support is probed separately from H.264 support.
The dispatcher can admit legacy API versions, but its API version alone does
not guarantee HEVC. The HEVC probe queries/initializes a real session and reads
VPS/SPS/PPS; it is capability evidence, not an encode/decode acceptance run.
QSV accepts positive even NV12 dimensions up to 8192, integer frame rates
1..240 and target bitrates up to 65535000 bps. Rate reset requires idle pending
input, then validates the active profile/tier/level, dimensions, format,
no-B-frame CBR bitrate and frame rate, refreshes parameter sets and requires
the next frame to be IDR. Failure closes that session.

## Build with the existing GN toolchain

Append these opt-in arguments to the existing build directory's `args.gn`:

```gn
rtc_use_nvenc = true
rtc_nvenc_include_dir = "<external-directory-containing-nvEncodeAPI.h>"
rtc_nvenc_build_tests = true
```

This requires Windows and `rtc_use_h264=true`. The first flag defaults to false;
the tests do not require enabling the full `rtc_include_tests` suite.
H.265 additionally uses the existing `rtc_use_h265` flag (true by default in
standalone WebRTC builds).

```powershell
gn gen out/ReleasePrefixed
gn check out/ReleasePrefixed "//modules/video_coding/codecs/external:*"
ninja -C out/ReleasePrefixed nvenc_validation
```

## QSV dispatcher and GN build

Match the dispatcher's CRT to the WebRTC output. The existing Release output
uses `/MT`. The initial dispatcher used `/MD`; rebuild into a separate directory
with CMP0091 set before the first compiler configuration. Do not suppress the
CRT mismatch at link time or alter the external source to bypass it:

```powershell
git clone --branch v2.14.0 https://github.com/intel/libvpl.git out/qsv-onevpl-src
# Verify HEAD is 025d43d086a3e663184cb49febe86152bf05409f and the source is clean.
cmake -S out/qsv-onevpl-src -B out/qsv-onevpl-mt-build `
  -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF `
  -DUSE_MSVC_STATIC_RUNTIME=ON -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
cmake --build out/qsv-onevpl-mt-build --config Release --target VPL
```

The rebuilt library's COFF directives were checked for `MT_StaticRelease` and
`LIBCMT`. For another CRT/configuration, rebuild and verify the matching library.
The source is used unchanged; no external/vendor source edits are needed.
Windows MSBuild needs access to Visual Studio/Windows SDK discovery information.
The application does not need the dispatcher build tools or SDK on the test host.

Append all four QSV opt-in GN arguments (the library path names a file):

```gn
rtc_use_qsv = true
rtc_qsv_include_dir = "F:/WebRTC/src/out/qsv-onevpl-src/api"
rtc_qsv_vpl_library = "F:/WebRTC/src/out/qsv-onevpl-mt-build/Release/vpl.lib"
rtc_qsv_build_tests = true
```

These require Windows, H.264 enabled and a CRT-compatible static dispatcher.
Adjust the absolute paths for the actual checkout. The flags do not require
enabling the full WebRTC test suite. NVENC may remain enabled in the same build.

```powershell
gn gen out/ReleasePrefixed
gn check out/ReleasePrefixed "//modules/video_coding/codecs/external:*"
ninja -C out/ReleasePrefixed webrtc qsv_validation
```

`qsv_h264_smoke` and `nvenc_h264_smoke` compile the same source with different
vendor selectors. They share 60 frames in two sessions, six fresh-decoder IDR
checks, serialized RTP/FU-A, SPS/PPS tracking, FFmpeg H.264 decoding, PSNR >= 28,
rate updates, pause/resume, timestamps and idempotent release.

## Intel host validation

Unpack the QSV H.264 validation package on a Windows x64 Intel graphics host:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_qsv_h264_validation.ps1
```

Return the complete newly generated `qsv-h264-validation-<timestamp>` evidence
directory, including `result.json`, frozen input manifests, logs and any `.dmp`.
The runner hashes itself and its binaries, records GPU/driver metadata and
uses a bounded child-process watchdog. `qsv_h264_smoke --require-qsv` requires
QSV hardware in both the direct adapter and builtin-factory path. Software
fallback is a hardware failure. `--software` exercises OpenH264 and explicitly
reports `SOFTWARE_HARNESS_ONLY QSV_NOT_RUN`; it is not QSV hardware acceptance.
Skipping unit tests leaves overall validation `INCOMPLETE`.

Package `20261005-v2` hardware validation is `FAIL`: on Intel UHD630 with
Media SDK API 1.35, runtime loading and QSV encoder initialization completed
before an access violation (`0xC0000005`). No crash dump was collected, so the
failing stack and root cause remain unconfirmed. Attaching the legacy
`GetHandle` result as an owned COM reference and releasing it during validation
is a strong ownership-related candidate, consistent with the log and FFmpeg's
legacy caller. The public Media SDK source inspected does not contain the
current Windows driver's D3D11 core, so it does not establish that driver's
exact return-reference contract or an API 1.x-wide guarantee.

Version `20261005-v3` retains an independent validation reference with `AddRef`
on the API 1 compatibility path; API 2 uses `Attach` according to the documented
added COM reference. The returned evidence
`out/qsv-hardware-evidence/qsv-h264-validation-20261005T141017881Z/assessment.json`
confirms the frozen v3 package's local hardware chain as `PASS` on Intel UHD630,
driver 31.0.101.2145 and Media SDK API 1.35. Its manifests, runner and both
executable hashes match the immutable v3 ZIP. All 46 unit tests passed; QSV
completed 60 frames across two sessions, six fresh-decoder IDR checks, rate
changes, pause/resume, restart and the builtin factory hardware path. Minimum
PSNR was 46.0697 dB. The earlier access violation did not recur in this run,
which supports the compatibility repair. The missing v2 crash stack, exact
driver ownership contract, possible retained-reference behavior on other API 1
drivers and long-term resource lifetime remain unconfirmed. The historical v2
`FAIL` and frozen v3 package metadata are retained unchanged.

The v3 QSV runner passes `--diagnostic-dir` to the same-source smoke executable.
An unhandled exception attempts to write a minidump into its fresh evidence
directory. `result.json` records dump names/sizes/hashes and the process exit
code; diagnostics and exit-code formatting do not change acceptance: both the
required unit test and `--require-qsv` smoke must exit successfully, without
software fallback. Dump absence is a diagnostic gap, not evidence of success.
The diagnostic-only `--diagnostic-selftest` intentionally raises a native
exception to check the dump writer. Its expected nonzero exit/dump is separate
from media validation and cannot satisfy the hardware PASS gate.

The QSV package contains the shared unit executable and QSV H.264 smoke, with a
statically linked CRT-compatible dispatcher and the existing H.264 decoder. It
does not deploy `vpl.lib` or a HEVC reference FFmpeg CLI. Build inputs, dispatcher
commit/API/header/library/license/CRT hashes and new executable hashes are frozen
in its manifests. Local build/unit/software success and earlier NVIDIA hardware
results do not establish QSV support for these new binaries.

## Freeze the v3 QSV package

Use Python 3's standard library tool from the source checkout. It performs no
build, tests, hardware runs or SDK changes. The normal sequence captures all
inputs before the affected build and fresh local validation:

```powershell
python modules/video_coding/codecs/external/package_qsv_h264_validation.py `
  --capture --package-version 20261005-v3
# Build out/ReleasePrefixed and collect fresh v3 local evidence.
# Write out/qsv-h264-local-validation-20261005-v3/result.json as below.
python modules/video_coding/codecs/external/package_qsv_h264_validation.py `
  --package-version 20261005-v3
```

The capture includes the root/media GN files, factory, license generator,
all external C++/headers/GN/Python/PowerShell/README/license inputs, `args.gn`,
SDK and license hashes. For this task, compilation may instead begin after a
separate `compile-inputs.json` ledger has frozen every C++/GN/args/SDK input;
the final capture then records only the later completion of runner/README/tool
metadata. The tool validates every ledger hash and its coverage of the current
compilation inventory and binds its hash in `build-inputs.json`. This preserves
the actual prebuild boundary without claiming the final capture preceded the
build or repeating unchanged successful local gates.

The new local gate result must contain at least:

```json
{
  "status": "PASS",
  "hardwareStatus": "NOT_RUN",
  "buildInputSnapshotSha256": "<SHA256 of gate/build-inputs.json>",
  "compileInputsSha256": "<SHA256 of gate/compile-inputs.json, when present>",
  "binaries": [
    { "file": "backend_video_encoder_unittests.exe", "sha256": "<current EXE SHA256>" },
    { "file": "qsv_h264_smoke.exe", "sha256": "<current EXE SHA256>" }
  ]
}
```

`PASS` here covers the new local build/unit/software gate only. The result is
written after the final capture; fresh evidence can be bound to the earlier
compilation ledger. Previous software/unit/binary PASS is not reused for v3.
Only SDK metadata is reused from the frozen v2 manifest: every listed SDK file
must exist at its real source path with the same size and SHA256, the complete
oneVPL API-header inventory and clean Git commit must match, and actual COFF
`.drectve` sections must identify `MT_StaticRelease` and `LIBCMT`. The new
manifest records that task-specific metadata source and the directive check.

Freeze rechecks inventory, hashes, GN args, SDK, licenses, ledger and both
EXE hashes against the local result. It creates
`out/qsv-h264-validation-20261005-v3`, its `.zip` and `.zip.sha256`, and refuses
to overwrite any existing frozen output or capture. The package contains the
two EXEs, runner, this README, manifests, verified license/notice copies and
small local `.json`/`.log`/`.txt` evidence. Diagnostic selftest `.dmp` files stay
in the local gate directory. Each ZIP entry uses `/` separators and is checked
for inventory, count, uncompressed bytes and SHA256 after writing. H.264 decode
uses builtin FFmpeg; no HEVC CLI, SDK library or driver DLL is bundled.

## Intel QSV H.265 validation and package

The QSV H.265 target is enabled by `rtc_use_qsv`, `rtc_qsv_build_tests` and the
existing `rtc_use_h265` flag. `qsv_validation` builds both QSV smoke executables
and the shared unit executable. H.265 uses the common HEVC RTP smoke source
with the QSV vendor selector; its builtin-factory path explicitly selects
`WebRTC-H265-HardwareEncoder/QSV/`.

Unpack the new Windows x64 Intel validation package and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_qsv_h265_validation.ps1
```

The runner requires the current shared units, a fresh QSV H.264 regression with
the new executable, and QSV H.265 hardware generation. It verifies the frozen
package files before launching them, records GPU/driver and executable hashes,
and passes an evidence directory to both smoke programs for crash dumps.
Missing hardware, software fallback, a crash, timeout, or failed content check
cannot produce hardware `PASS`. `-SkipUnitTests` leaves overall `INCOMPLETE`.
Return the full new `qsv-h265-validation-<timestamp>` directory, including the
manifests, logs, RTP/HEVC artifacts, content measurements and any `.dmp` files.

HEVC validation exports the real serialized/depacketized RTP stream as Annex B,
covering 60 frames in two sessions, timestamps, rate changes, pause/resume and
restart. An independent software FFmpeg CLI checks all 60 decoded frames and
six required IDRs in separate decoder processes, plus the explicit QSV builtin
factory output. Dimensions/frame counts must match and every frame must have
PSNR >= 28 dB. The reference CLI is bundled under `reference-ffmpeg/`, with its
original license/README under `licenses/`; `-FfmpegPath` may select another
reference tool, whose process hash is recorded. This CLI is an acceptance tool,
not a linked WebRTC dependency or a product HEVC decoder.

`-SoftwareFixture <HEVCFX01 fixture>` is a diagnostic mode for the same RTP and
independent-decoder measurement chain, with H.264 using OpenH264. A fixture is
produced independently by the local reference encoder rather than the product
QSV backend. This mode requires `ReplayFixture`, `hardware=false` and explicit
`SOFTWARE_FIXTURE_ONLY` metadata. The QSV builtin-factory gate is `NOT_RUN`;
success is `SOFTWARE_PASS` with `hardwareStatus=NOT_RUN`, never hardware `PASS`.
It is not the default hardware runner mode. Crash dump selftests are also
diagnostic-only and cannot satisfy hardware acceptance.

The immutable `20261005-v1` local gate passed its affected Release build and all
51 shared unit tests. Its software fixture completed 60 RTP-reconstructed
frames and six independent IDR decodes with minimum stream PSNR 63.56 dB. Its
diagnostic selftest preserved the deliberate native exception and generated a
verified dump. Local QSV HEVC availability was `SKIP` (exit 77: no Intel adapter).
Those results remain bound to
`out/qsv-h265-local-validation-20261005-v1/result.json`.

The returned v1 Intel UHD630/API 1.35 evidence
`out/qsv-hardware-evidence/qsv-h265-validation-20261005T145831703Z/assessment.json`
is `FAIL` for H.265 hardware. The 51 unit tests and the current H.264 hardware
regression passed, but HEVC initialization exited with code 1 before producing
any frames. The error reports a required profile/tier/level/NV12/no-B/rate
configuration mismatch during initialization readback. It does not identify
the individual mismatching field. This was a handled initialization failure,
not an access violation or watchdog timeout; absence of a dump does not change
the hardware failure. The exact mismatch remains unconfirmed.

The returned `20261005-v2` Intel UHD630/API 1.35 evidence
`out/qsv-hardware-evidence/qsv-h265-validation-20261005T153555555Z/assessment.json`
is still `FAIL` for H.265 hardware. All 60 unit tests and the v2 H.264 hardware
regression passed. HEVC initialization now passed with SDK/SPS coded dimensions
640x368 and SPS display dimensions 640x360, supporting the v2 dimension-contract
repair. The later bitrate reset failed with vendor status -14 and the smoke
exited with code 1; the independent HEVC decode gate remained `NOT_RUN`. The
v1/v2 packages, local gates and returned `FAIL` evidence are preserved.

The returned `20261005-v3` evidence
`out/qsv-hardware-evidence/qsv-h265-validation-20261005T155522454Z/assessment.json`
confirms the frozen v3 QSV H.265 local hardware chain and same-version QSV H.264
regression as `PASS` on Intel UHD630, driver 31.0.101.2145 and API 1.35. The
package, runner, three executables and input-snapshot hashes match the frozen
v3 package; its C++ compilation inputs matched the assessed v3 source boundary. All
60 unit tests passed. QSV H.265 completed 60 frames across two sessions and six
independent IDR decodes, with minimum stream PSNR 51.98 dB. Its explicit QSV
builtin-factory output passed independent one-frame decoding at 54.58 dB.
The QSV H.264 hardware regression completed 60 frames and its factory gate,
with minimum PSNR 46.0697 dB.

The 104 returned evidence files were archived with byte/hash verification;
all 41 artifact hashes, existing decoded I420 frame hashes and PSNR records
were checked offline. No build or runtime rerun was needed. This returned
acceptance is separate from the immutable v3 package's original local
`hardwareStatus=NOT_RUN`; that frozen metadata and the v1/v2 `FAIL` evidence
remain unchanged.

The v2 reset error is `MFX_ERR_INCOMPATIBLE_VIDEO_PARAM` (-14). The oneVPL
`MFXVideoENCODE_Reset` contract states that this status requires additional
allocation and the component should be closed and reinitialized. The backend
previously aborted without that reinitialization path, which is a confirmed
handling defect. The driver field or allocation limit that triggers -14 is
still unknown. The v3 evidence also shows -14 for the bitrate-only change from
250 to 350 kbps at unchanged 15 fps, so the rejection cannot be attributed only
to a frame-rate change.

Version v3 handles only the HEVC -14 reset result by copying the requested
configuration and calling the existing full `Open` path. It closes the old
encoder/session/device, recreates them with new storage, reads the active
VPS/SPS/PPS, repeats strict configuration validation and requires an IDR before
continuing through the existing WebRTC adapter. This can replace the vendor
session/device; it does not promise to preserve that session. Other reset
errors retain their failure behavior, and the H.264 reset logic is unchanged.
The runner still requires the same hardware identities, frames, rate/pause
transitions, independent IDR decodes, dimensions and PSNR. The returned v3 run
exercised four actual -14 close/reinitializations and passed: each session's
changes to 250 kbps/15 fps and then 350 kbps/15 fps were confirmed by active
readback. Recovery frames 15, 20, 45 and 50 contained NAL types
`[32, 33, 34, 39, 19]`, including in-band VPS/SPS/PPS and an IDR. Independent
decoding and content checks passed after recovery. This confirms the missing
SDK-required reinitialization handling was fixed on this host; it does not
identify the driver's internal allocation constraint.

Reset diagnostics now include `QSV_CONFIG stage=ResetInput` and `ResetResult`,
along with maximum bitrate, buffer size and initial delay. The reinitialization
path reports `QSV_RECONFIG ... action=close_reinitialize`. SPS dimensions use
literal `x` separators, correcting the v2 diagnostic's printed character code
`120`; this formatting fix does not change media acceptance.

The v3 local gate passed the affected Release `qsv_validation`,
`nvenc_validation` and `webrtc` build and all 60 shared unit tests. The previous
successful GN generation/checks were reused after their GN/args/include-graph
inputs were verified unchanged; the changed C++ input was rebuilt. The software
runner reported `SOFTWARE_PASS` with 60 HEVC RTP-reconstructed frames, six
independent IDR decodes, minimum stream PSNR 63.56 dB and a passing OpenH264
regression. Its PowerShell 5 GPU metadata access denial is retained. Native
availability independently reported `SKIP` (exit 77: no Intel adapter), with no
encoder selected by the explicit QSV factory. The diagnostic selftest wrote a
verified dump while preserving its deliberate native exception. The evidence
is under `out/qsv-h265-local-validation-20261005-v3`; these local/diagnostic
results did not exercise the Intel driver's reset/reinitialization path. Intel
hardware was `NOT_RUN` at this local freeze; the separate returned v3 acceptance
above now qualifies QSV H.265 and the same-version H.264 local hardware chain.
The current v3 NVIDIA hardware regression remains `NOT_RUN`.

Source inspection identified a v1 validation defect: HEVC coded luma dimensions
were required to equal the requested display dimensions. A coded picture can
include padding that its SPS conformance window removes from the display
picture. Version v2 jointly validates the active SPS and SDK readback: the SPS
display dimensions must equal the requested width/height; the SDK HEVC coded
dimensions, when that extension is requested, must equal the SPS coded
dimensions; the coded and display dimensions must fit the aligned storage
surface. An aligned session without the HEVC extension obtains its coded size
from the active SPS. FrameInfo crop must still exactly match the request with
zero offset. This fixes the confirmed coded/display assumption without claiming
it was the particular field that rejected the returned v1 run.

HEVC initialization and rate reset now read the active VPS/SPS/PPS before the
configuration check. The SPS dimension parser requires Main profile, Main tier,
8-bit luma/chroma, 4:2:0 and a single temporal layer. The existing no-B-frame,
IDR interval, CBR bitrate, frame-rate and storage constraints remain mandatory;
RTP, independent-decoder frame/dimension checks, six IDR checks and PSNR >= 28
remain unchanged. A malformed or incompatible SPS cannot satisfy the new
dimension check.

The v2 stderr records `QSV_CONFIG` stage and vendor status with requested and
returned profile/level, storage/crop, coded dimensions, bit depth, rate, frame
rate and ordering fields, plus the active SPS coded/display dimensions. A
readback rejection lists each failing field in `mismatch=` instead of the v1
aggregate error. These diagnostics can identify any remaining Intel-host
incompatibility; they do not substitute for successful hardware generation and
independent content validation.

The v2 local gate passed fresh GN generation/checks and the affected Release
`qsv_validation`, `nvenc_validation` and `webrtc` build. All 60 shared unit tests
passed, including nine new SPS/dimension-contract tests. The software runner
reported `SOFTWARE_PASS`: 60 HEVC RTP-reconstructed frames, six independent IDR
decodes, minimum stream PSNR 63.56 dB and the OpenH264 software regression passed.
Its GPU metadata collection reported access denied; it does not provide Intel
hardware evidence. The native availability check independently reported
`SKIP` (exit 77: no Intel adapter), and the explicit QSV factory selected no
encoder. The diagnostic selftest preserved the deliberate `0xE0425356`
exception and wrote a verified dump. These are local or diagnostic results in
`out/qsv-h265-local-validation-20261005-v2`; they remain separate from the v2
Intel hardware `FAIL` described above. NVIDIA hardware regression was `NOT_RUN`.

Freeze the task's new package using Python 3's standard library:

```powershell
python modules/video_coding/codecs/external/package_qsv_h265_validation.py `
  --capture --package-version 20261005-v3
# Build and collect fresh local units/software/availability evidence.
# Write out/qsv-h265-local-validation-20261005-v3/result.json.
python modules/video_coding/codecs/external/package_qsv_h265_validation.py `
  --package-version 20261005-v3
```

The capture binds all current external source/runner/tool/README inputs, root
and media GN/factory inputs, actual GN args, oneVPL SDK/static library and
licenses. A separately frozen prebuild `compile-inputs.json` ledger allows
runner/README metadata to finish later: every compilation input and coverage
must still match; the final result binds both snapshot hashes. New binary PASS
is required for this version. The tool prefers SDK/license metadata from the
unchanged H.264 v3 ZIP. When that archive is absent, it accepts only the immutable
H.265 v1 ZIP with SHA256
`AC3F4A52F11CFCDB6698A79DF1BB0DC6DED53E3EE5D07046350DC5D91FFC56C9`.
It records the selected archive and inherited metadata provenance while
rechecking real dependency bytes, the full API-header inventory, clean oneVPL
commit and actual COFF `/MT` directives. Only the original seven SDK/runtime
license files are reused; the two reference FFmpeg license/README files are
verified against the current original installation. No earlier executables or
validation PASS are reused. The tool never builds, tests, downloads or alters
external SDK sources.

The local result requires `status=PASS`, `hardwareStatus=NOT_RUN`,
`buildInputSnapshotSha256`, `compileInputsSha256` when present, hashes for
`backend_video_encoder_unittests.exe`, `qsv_h264_smoke.exe` and
`qsv_h265_smoke.exe`, and `referenceFfmpegSha256`. The package tool locates the
existing standalone static CLI on PATH, or accepts `--ffmpeg-path`; the
installation root must contain its original `LICENSE` and `README.txt`.
Freeze rechecks all bindings, creates
`out/qsv-h265-validation-20261005-v3`, `.zip` and `.zip.sha256`, and verifies
every portable ZIP entry's count, bytes and SHA256. It refuses to overwrite
frozen outputs or captures. The default version is v3; v1/v2 remain explicit
version choices without permission to rewrite their frozen outputs. The runner
accepts only the exact QSV H.265 validation scope for v1/v2/v3, and the hashed input
snapshot version must match the package version. Local `.dmp` selftest files and
raw fixture frames remain outside the shipping package. The frozen v3 package
has separate returned Intel-host acceptance above. Future source/binary versions
need their own hardware evidence; previous H.264 v3 and v1/v2 H.264/unit PASS do
not qualify another package. Frozen local metadata is not rewritten after a
returned hardware assessment.

## x265 software H.265 backend and build

The x265 backend reuses `VideoEncoderBackend` and `BackendVideoEncoder`; it is
not the QSV validation-only `ReplayFixture` or the independent reference FFmpeg
encoder. It converts the borrowed NV12 input into owned I420 planes for the
real x265 C API. Each successful encode must synchronously return exactly one
complete Annex B picture with the original frame ID. The backend copies x265's
NAL bytes before the next library call invalidates their pointers. It checks
Main profile/Main tier, 8-bit 4:2:0, negotiated SPS level, coded/display
sizes, no B frames, no lookahead, one temporal layer, and real IDR requests.
Close retires the encoder before releasing host planes and parameters; it does
not call process-global `x265_cleanup` while other encoders may exist.

The current task-owned dependency is x265 4.1, clean source commit
`1d117bed4747758b51bd2c124d738527e30392cb`, C API build 215. It is built as a
Windows x64, 8-bit static MSVC Release library with `/MT`. The existing Visual
Studio CMake 3.31.6 is used: CMake 4.3 rejects the upstream source's OLD policy
settings, so the build uses the compatible installed tool without changing
vendor source or suppressing CRT conflicts. The current source is under
`out/x265-src`; installed API headers and library are under
`out/x265-mt-install`.

For that unchanged source, the configuration used is:

```powershell
$x265Cmake = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $x265Cmake -S out/x265-src/source -B out/x265-mt-build-v1 `
  -G "Visual Studio 17 2022" -A x64 -DSTATIC_LINK_CRT=ON `
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DHIGH_BIT_DEPTH=OFF -DEXPORT_C_API=ON `
  -DENABLE_ASSEMBLY=ON -DNASM_EXECUTABLE=F:/WebRTC/src/out/ReleasePrefixed/nasm.exe `
  -DENABLE_HDR10_PLUS=OFF -DENABLE_SVT_HEVC=OFF -DENABLE_TESTS=OFF -DNATIVE_BUILD=OFF
& $x265Cmake --build out/x265-mt-build-v1 --config Release --target x265-static
& $x265Cmake --install out/x265-mt-build-v1 --config Release --prefix F:/WebRTC/src/out/x265-mt-install
```

Adjust the Visual Studio edition and absolute paths for the actual machine.
The generated library's real COFF CRT directives must match WebRTC's output;
another configuration needs its own matching build. x265's API build number,
bit depth and parameter/picture structure sizes are checked before opening
an encoder. No x265 CLI or DLL is required on the validation host.
The backend target defines the public header's disabled Alpha/multiview/SCC/VMAF
features as zero and bit depth as eight, matching this library without disabling
WebRTC's `-Wundef` or editing the vendor header.

Append the opt-in GN configuration to the existing build's `args.gn`:

```gn
rtc_use_x265 = true
rtc_x265_include_dir = "F:/WebRTC/src/out/x265-mt-install/include"
rtc_x265_library = "F:/WebRTC/src/out/x265-mt-install/lib/x265-static.lib"
rtc_x265_license_file = "F:/WebRTC/src/out/x265-src/COPYING"
rtc_x265_build_tests = true
```

The backend defaults to disabled, currently requires Windows and
`rtc_use_h265=true`, and does not require the full `rtc_include_tests` suite.
The current acceptance build retains NVENC/QSV and H.264 support; it can run
the OpenH264 regression without a NVIDIA or Intel device.

```powershell
gn gen out/ReleasePrefixed
gn check out/ReleasePrefixed "//modules/video_coding/codecs/external:*"
ninja -C out/ReleasePrefixed webrtc x265_validation
```

The builtin factory can explicitly select software encoding:

```cpp
auto env = CreateEnvironment(std::make_unique<FieldTrials>(
    "WebRTC-H265-Encoder/x265/"));
auto encoder = CreateBuiltinVideoEncoderFactory()->Create(env, format);
```

Without an explicit selection, H.265 creation tries NVENC, QSV, then x265.
The existing exact hardware selections `WebRTC-H265-HardwareEncoder/QSV/`
and `/NVENC/` retain their vendor restriction; missing hardware cannot be
reported as a successful software substitution. Availability probing opens
and validates a real x265 session and SPS; it does not encode a frame or prove
media quality. x265 reports software identity and does not by itself establish
power efficiency or real-time performance.

The software backend accepts even NV12 dimensions from 16 through 8192,
integer frame rates 1..240 and positive target bitrates up to 65535000 bps.
The `ultrafast`/`zerolatency` configuration uses one frame thread, no worker
pool, no B frames or lookahead, and ABR with target-equal VBV maximum/buffer
and strict-CBR control. These are effective configuration checks, not a
measurement of real network bitrate accuracy. Any effective bitrate or FPS
change closes and reopens the encoder, validates its new configuration and
SPS, and requires the next output to be IDR. x265 4.1's reconfigure path does
not copy `fpsNum`/`fpsDenom`; reopening avoids claiming an FPS update that the
active encoder has not applied. The shared adapter owns zero-bitrate pause,
resume and frame/timestamp propagation.

## x265 software validation and immutable package

Unpack `out/x265-validation-20261006-v1.zip` into a new Windows x64 directory
and run the package's PowerShell 5-compatible entry:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_x265_validation.ps1
```

`x265_h265_smoke.exe` is built from the same HEVC adapter/RTP exporter as the
NVENC/QSV smokes with the x265 selector. It requires real `x265` software
identity and the explicit x265 builtin factory. It exposes no replay-fixture
mode. The runner requires fresh shared units, `nvenc_h264_smoke.exe --software`
for the OpenH264 regression, and 60 x265 frames in two sessions. It verifies
serialized/depacketized RTP and frame IDs, FU coverage, rate/FPS changes,
pause/resume and restart. The independent hash-bound FFmpeg CLI fully decodes
all 60 frames, each of six required IDRs in a new process, and a fresh builtin
factory frame; exact dimensions/frame counts and per-frame PSNR >= 28 dB are
mandatory. This CLI is a validation tool, not the product HEVC decoder.

Success is `X265_PASS`, with `softwareEncoderStatus=PASS` and
`hardwareStatus=NOT_APPLICABLE`. A replay fixture cannot qualify, and
`-SkipUnitTests` leaves `INCOMPLETE`. A failed identity, missing hash binding,
crash, timeout or media/content check remains a failure. The process watchdog
is an acceptance bound, not a product encoder teardown guarantee. `-FfmpegPath`
may select a reference executable only when its size/SHA matches the one
bound by the package manifest. Return the complete fresh
`x265-validation-<timestamp>` evidence directory for offline review.

The `20261006-v1` affected Release build and real local software-media gate
passed. All 72 shared tests passed, including eight actual x265 backend tests
and four creation-policy tests. Real x265 generated 60 RTP-reconstructed frames
across two sessions, 95 FU packets and 11 AP packets. Independent FFmpeg decoded
all 60 frames (minimum PSNR 52.59 dB), six required IDRs in separate processes
and the builtin factory's fresh frame (55.15 dB). OpenH264 regression passed.
The report is `out/x265-local-validation-20261006-v1/x265-chain/result.json`
with `X265_PASS`; all 41 generated media-artifact hashes were verified offline.
The successful combined and x265-only GN checks were reused after their
GN/args/header/SDK inputs were verified unchanged for the last backend repair.
The PowerShell 5 GPU metadata access denial remains recorded; it does not
affect the software-only identity/encoding/content checks. Hardware is
`NOT_APPLICABLE`, not a new NVENC/QSV hardware qualification. Previous QSV
libx265 fixture runs, NVENC/QSV hardware PASS, and older shared-unit executables
do not qualify this implementation. Real network/PeerConnection/RTCP PLI, bitrate
accuracy, real-time FPS/latency, long stability and other CPU/platform coverage
are also `NOT_RUN`. The product builtin decoder still has no HEVC support.

The first real media attempt failed before encoding: x265 4.1's
`x265_copy_params` only copies `numScalableLayers` with Alpha support enabled.
Its `encoder_parameters` therefore left the caller's zero-initialized field
at zero in this Alpha-disabled build. Readback now first calls `param_default`,
following the vendor's own default-then-copy contract. The single-layer==1
predicate and independent SPS/NAL layer checks remain required. The original
failed result is preserved in `out/x265-preparation-attempt-03/x265-chain`.

The task-specific packager reuses the existing hash/COFF/ZIP and combined-build
SDK/license primitives, then rechecks the new x265 dependency metadata,
source Git commit/clean state, exact `x265.h`/`x265_config.h` headers, static
library bytes/API/CRT and original `COPYING`. Supply the verified dependency
metadata JSON and existing reference CLI explicitly:

```powershell
python modules/video_coding/codecs/external/package_x265_validation.py `
  --prepare-local-runner --package-version 20261006-v1 `
  --x265-metadata out/x265-dependency-metadata.json `
  --ffmpeg-path D:/software/ffmpeg/bin/ffmpeg.exe
# Run the generated local-runner once, using a fresh evidence directory outside
# local-runner, after its verified compile-inputs.json ledger and affected build.
# Finalize README/tool metadata, then capture the immutable final inputs.
python modules/video_coding/codecs/external/package_x265_validation.py `
  --capture --package-version 20261006-v1 `
  --x265-metadata out/x265-dependency-metadata.json `
  --ffmpeg-path D:/software/ffmpeg/bin/ffmpeg.exe
# Write out/x265-local-validation-20261006-v1/result.json bound to the real gate.
python modules/video_coding/codecs/external/package_x265_validation.py `
  --package-version 20261006-v1 `
  --x265-metadata out/x265-dependency-metadata.json `
  --ffmpeg-path D:/software/ffmpeg/bin/ffmpeg.exe
```

`--prepare-local-runner` requires and verifies the prebuild compilation ledger;
it creates a hash-bound `LOCAL_BUILD_GATE` stage and independent local input
snapshot without claiming final package publication. The final capture also
rechecks every compile input, so README metadata can finish after a passing
unchanged compilation/media run. The final summary requires `status=PASS`,
`softwareEncoderStatus=PASS`, `hardwareStatus=NOT_APPLICABLE`, final snapshot
and compile-ledger hashes, current hashes for the shared-unit, H.264 and x265
EXEs, `referenceFfmpegSha256`, and `runnerResultFile`/`runnerResultSha256`.
Freeze rechecks the actual runner's `X265_PASS`, binary/runner/reference hashes
and source inputs rather than accepting only a summary status. It writes
`out/x265-validation-20261006-v1`, its `.zip` and `.zip.sha256`, verifies every
portable ZIP entry's inventory, bytes and SHA, and refuses to overwrite any
frozen output/capture. The final `FROZEN_PACKAGE` contains the three EXEs,
runner, manifests, this README, verified license copies, reference FFmpeg CLI
and small local evidence; SDK libraries, vendor DLLs and raw frames are omitted.

x265 is licensed under GPL-2.0-or-later or a separately obtained commercial
license. The original `COPYING` is passed to the GN license metadata and
packaged as `licenses/x265-COPYING`; WebRTC's license does not replace it.
The current `webrtc` GN graph's generated third-party notice also includes x265
and is packaged separately from the inherited SDK reference notices. GN's
default `desc` omits metadata; the generator explicitly queries the linked
x265 backend's metadata instead of silently omitting this external static lib.
A linked product must resolve its applicable distribution/license obligations
before release. No commercial license or product release qualification is
provided by this change or by a validation `X265_PASS`. Downstream consumers
must explicitly link the matching `x265-static.lib`; the complete static
`webrtc.lib` does not promise to absorb this external library.

## NVIDIA host validation

Unpack the generated validation package on a Windows x64 NVIDIA host with a
driver that supports NVENC API 12.2. No SDK installation is required on that host.
Run from PowerShell in the extracted directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_nvenc_validation.ps1
```

The runner records GPU/driver information, binary hashes, logs and exit codes in
an evidence directory. Return that directory to assess the hardware result.
`nvenc_h264_smoke --require-nvenc` rejects unavailable hardware and software
fallback. `--software` checks the same RTP/FFmpeg measurement chain with OpenH264
and reports software validation separately.

The H.265 validation package also contains `nvenc_h265_smoke.exe` and
`run_nvenc_h265_validation.ps1`. Run the latter in the extracted package.
It runs adapter unit tests, NVENC H.264 regression, and NVENC H.265 generation
through actual H.265 RTP serialization/depacketization/Annex B reassembly.
An independent FFmpeg CLI decodes the resulting 60-frame stream and each
required keyframe in a new process, checking dimensions, frame count and PSNR.
The CLI is a validation dependency; it is not linked into `webrtc.lib`.
The runner can use a bundled FFmpeg or `-FfmpegPath <path-to-ffmpeg.exe>`.
The product's current Windows builtin decoder and bundled FFmpeg library do
not support HEVC decoding; this change adds the encoder, not an H.265 receiver.

The focused gate covers encoding, H.264 RTP packetization/depacketization,
SPS/PPS tracking, access-unit assembly and FFmpeg decoding. It checks motion
content, timestamps, dimensions, frame counts, PSNR, explicit IDR requests,
rate changes, pause/resume and repeated initialization/release.

This is an in-process codec/RTP roundtrip. PeerConnection signaling, real network
transport, RTCP feedback, congestion control, long stability and device-loss
recovery require subsequent runtime validation. NVENC CPU staging-map polling
has a 200 ms deadline. NVENC uses synchronous blocking bitstream locks, as OBS
and FFmpeg do; encoding, bitstream locking and driver teardown have no hard
deadline. The validation runner's process timeout does not provide such a
guarantee for the product's encoder sequence.
H.265 hardware acceptance must be collected for its own source/binary version;
the earlier H.264 PASS does not establish H.265 support. Each new source/binary
version needs its own hardware evidence. The new product x265 encoding path has
its own software acceptance boundary; earlier replay-fixture results do not
validate that encoder or its newly compiled factory/binaries.
QSV H.265 v1/v2 hardware acceptance is `FAIL`; returned v3 Intel QSV H.265 and
same-version H.264 local hardware acceptance is `PASS` on the UHD630/API 1.35
host above. The current v3 NVIDIA hardware regression remains `NOT_RUN`.
QSV H.264 v2 hardware acceptance is `FAIL`; the frozen v3
package's local hardware chain is `PASS` on the UHD630/API 1.35 host above.
Real network/RTCP PLI, bitrate accuracy, real-time FPS and
latency, long stability, multi-GPU/driver coverage and real device-loss recovery
are also `NOT_RUN`. QSV uses serialized synchronous vendor operations; its
validation watchdog does not prove bounded product cancellation or teardown.
