/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_HARDWARE_ENCODER_METRICS_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_HARDWARE_ENCODER_METRICS_H_
#define WEBRTC_LINUX_HARDWARE_ENCODER_METRICS 1
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace webrtc::external_encoder {
// Linux-only process counters, never per-publication attribution. Every array
// has a fixed capacity. Snapshots are observational: writers remain lock free
// and fields are not a transactionally consistent multi-counter sample.
enum class LinuxEncoderMetricsScope { kProcess };
enum class LinuxEncoderMetricBackend { kNvenc, kQsv, kVaapi, kCount };
enum class LinuxEncoderMetricTransfer {
  kOwnedNv12Staging, kCudaUpload, kQsvCpuSurfaceCopy, kVaapiUpload, kCount
};
enum class LinuxEncoderMetricOperation { kOpen, kSetRates, kEncode, kClose, kShutdown, kCount };
inline constexpr std::array<uint64_t, 8> kLinuxEncoderDurationUpperBoundsNs = {
    1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, UINT64_MAX};
struct LinuxEncoderDurationSnapshot {
  uint64_t samples = 0, total_ns = 0, max_ns = 0;
  std::array<uint64_t, kLinuxEncoderDurationUpperBoundsNs.size()> buckets{};
};
struct LinuxEncoderTransferSnapshot {
  // False means this build has no hook for the transfer; zero counters must
  // then be reported as Unknown, not as a measured absence of copying.
  bool measurement_available = false;
  uint64_t started_calls = 0, requested_bytes = 0;
  uint64_t successful_calls = 0, successful_bytes = 0;
  LinuxEncoderDurationSnapshot completed;
};
struct LinuxEncoderOperationSnapshot {
  uint64_t started_calls = 0, timeouts = 0;
  LinuxEncoderDurationSnapshot queue_wait, service;
};
struct LinuxEncoderPoolSnapshot {
  bool measurement_available = false;
  uint64_t allocation_calls = 0, allocated_surfaces = 0;
  // owned includes retained. active = owned - retained at a quiescent sample.
  uint64_t owned_pools = 0, owned_surfaces = 0, owned_bytes = 0;
  uint64_t retained_pools = 0, retained_surfaces = 0, retained_bytes = 0;
  uint64_t rebuild_started = 0, rebuild_succeeded = 0, rebuild_failed = 0;
};
struct LinuxEncoderWorkerSnapshot {
  uint64_t capacity = 0, occupied_slots = 0, running = 0, retiring = 0, retained_slots = 0;
  uint64_t started = 0, finished = 0, thread_start_failures = 0, capacity_rejections = 0;
};
struct LinuxEncoderBackendSnapshot {
  uint64_t created_instances = 0;
  uint64_t open_attempts = 0;
  uint64_t open_succeeded = 0;
};
struct LinuxHardwareEncoderMetricsSnapshot {
  uint32_t schema_version = 1;
  LinuxEncoderMetricsScope scope = LinuxEncoderMetricsScope::kProcess;
  std::array<LinuxEncoderTransferSnapshot, static_cast<size_t>(LinuxEncoderMetricTransfer::kCount)> transfers{};
  std::array<LinuxEncoderOperationSnapshot, static_cast<size_t>(LinuxEncoderMetricOperation::kCount)> operations{};
  std::array<LinuxEncoderPoolSnapshot, static_cast<size_t>(LinuxEncoderMetricBackend::kCount)> pools{};
  std::array<LinuxEncoderBackendSnapshot, static_cast<size_t>(LinuxEncoderMetricBackend::kCount)> backends{};
  LinuxEncoderWorkerSnapshot workers;
  // There is no readback measurement hook in this candidate. Unknown must not
  // be serialized as measured zero. No GPU/driver identity strings are logged.
  std::optional<uint64_t> gpu_readback_bytes;
};
LinuxHardwareEncoderMetricsSnapshot SnapshotLinuxHardwareEncoderMetrics();
uint64_t LinuxEncoderElapsedNs(std::chrono::steady_clock::time_point start);
// Scope on the actual operation, including driver failure/hang. started is
// recorded before work, completion only when it returns. Timers allocate none.
class LinuxEncoderTransferTimer {
 public:
  LinuxEncoderTransferTimer(LinuxEncoderMetricTransfer kind, uint64_t bytes);
  ~LinuxEncoderTransferTimer();
  LinuxEncoderTransferTimer(const LinuxEncoderTransferTimer&) = delete;
  LinuxEncoderTransferTimer& operator=(const LinuxEncoderTransferTimer&) = delete;
  void Finish(bool successful);
 private:
  LinuxEncoderMetricTransfer kind_;
  uint64_t bytes_;
  std::chrono::steady_clock::time_point start_;
  bool finished_ = false;
};
void RecordLinuxEncoderOperationStarted(LinuxEncoderMetricOperation, uint64_t queue_ns);
void RecordLinuxEncoderOperationCompleted(LinuxEncoderMetricOperation, uint64_t service_ns);
void RecordLinuxEncoderOperationTimeout(LinuxEncoderMetricOperation);
enum class LinuxEncoderWorkerEvent {
  kReserved, kReservationReleased, kStarted, kFinished, kRetiring,
  kRetirementFinished, kRetained, kThreadStartFailed, kCapacityRejected
};
void RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent);
void SetLinuxEncoderWorkerCapacity(uint64_t capacity);
void RecordLinuxEncoderPoolAllocated(LinuxEncoderMetricBackend, uint64_t surfaces, uint64_t bytes);
void RecordLinuxEncoderPoolRetained(LinuxEncoderMetricBackend, uint64_t surfaces, uint64_t bytes);
void RecordLinuxEncoderPoolReleased(LinuxEncoderMetricBackend, uint64_t surfaces, uint64_t bytes, bool retained);
// Raw backend entry-point attribution, independent of outer-worker operations.
// Failed Open (including unavailable library/device) is still an attempt. A
// rates rebuild has its separate counter and is not a new public Open call.
void RecordLinuxEncoderBackendCreated(LinuxEncoderMetricBackend);
void RecordLinuxEncoderBackendOpenAttempt(LinuxEncoderMetricBackend);
void RecordLinuxEncoderBackendOpenSucceeded(LinuxEncoderMetricBackend);
void RecordLinuxEncoderRebuildStarted(LinuxEncoderMetricBackend);
void RecordLinuxEncoderRebuildCompleted(LinuxEncoderMetricBackend, bool successful);
class LinuxEncoderRebuildTimer {
 public:
  explicit LinuxEncoderRebuildTimer(LinuxEncoderMetricBackend backend);
  ~LinuxEncoderRebuildTimer();
  LinuxEncoderRebuildTimer(const LinuxEncoderRebuildTimer&) = delete;
  LinuxEncoderRebuildTimer& operator=(const LinuxEncoderRebuildTimer&) = delete;
  void Succeeded() { successful_ = true; }
 private:
  LinuxEncoderMetricBackend backend_;
  bool successful_ = false;
};
}  // namespace webrtc::external_encoder
#endif
