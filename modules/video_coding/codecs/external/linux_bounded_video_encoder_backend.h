/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_BOUNDED_VIDEO_ENCODER_BACKEND_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_BOUNDED_VIDEO_ENCODER_BACKEND_H_

#define WEBRTC_LINUX_BACKEND_RETIREMENT_REGISTRY 1

#include <chrono>
#include <cstddef>
#include <memory>

#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder {

// Candidate caller-wait limits, not performance acceptance thresholds. They
// include worker scheduling and must be tuned against real Linux devices.
struct LinuxBackendDeadlines {
  std::chrono::milliseconds open{5000};
  std::chrono::milliseconds set_rates{1000};
  std::chrono::milliseconds encode{1000};
  std::chrono::milliseconds close{2000};
};

// Linux only: one task slot, one reusable owned NV12 buffer, one driver owner.
// GetInfo uses the supplied immutable snapshot; no caller invokes the inner
// backend. The inner object must be inert (no driver work) when passed here.
// A checked pthread startup/capacity failure precedes any driver call and is
// reported by Open as kUnavailable. Once started, the worker owns all inner
// calls and its destruction, including late cleanup after caller destruction.
//
// Wait timeout cannot cancel a driver. It seals the instance and permanently
// poisons the vendor with kRetirementFailed; late output is never delivered.
// A fixed process-lifetime registry owns joinable workers and their storage.
// Running threads are neither detached nor joined with an unbounded wait.
// Native driver crashes are not isolated by this thread boundary.
std::unique_ptr<VideoEncoderBackend> CreateLinuxBoundedBackend(
    std::unique_ptr<VideoEncoderBackend> backend,
    HardwareEncoderVendor vendor,
    Info cached_info,
    LinuxBackendDeadlines deadlines = {});

// Shared Linux fail-closed gate across vendor and independent backend policies.
// Record is sticky for the process lifetime; no reset or mutable reason cache.
// Backend-specific coordinators retain their existing typed diagnostics.
Status CheckLinuxBackendRetirement();
void RecordLinuxBackendRetirementFailure(std::string_view reason);

// Optional Linux backend namespace independent of the Windows vendor enum.
// Both functions must be non-null and remain valid for the process lifetime.
// check returns kOk or sticky kRetirementFailed; record must be thread safe.
struct LinuxBackendRetirementHooks {
  Status (*check)() = nullptr;
  void (*record)(std::string_view) = nullptr;
};
std::unique_ptr<VideoEncoderBackend> CreateLinuxBoundedBackendWithRetirementHooks(
    std::unique_ptr<VideoEncoderBackend> backend,
    Info cached_info,
    LinuxBackendRetirementHooks hooks,
    LinuxBackendDeadlines deadlines = {});

namespace internal {
// Deterministic startup failure before any driver call; no global hook.
std::unique_ptr<VideoEncoderBackend> CreateLinuxBoundedBackendForTesting(
    std::unique_ptr<VideoEncoderBackend> backend,
    HardwareEncoderVendor vendor,
    Info cached_info,
    LinuxBackendDeadlines deadlines,
    bool fail_thread_creation);
// Reaps only completed pthreads, without waiting. Includes active, retiring,
// and permanently retained cleanup-failure slots in the returned count.
size_t LinuxBoundedWorkerCountForTesting();
size_t LinuxBoundedWorkerCapacityForTesting();
}  // namespace internal
}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_BOUNDED_VIDEO_ENCODER_BACKEND_H_
