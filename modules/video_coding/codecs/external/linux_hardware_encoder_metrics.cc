/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"
#include <atomic>
namespace webrtc::external_encoder {
namespace {
using Counter = std::atomic<uint64_t>;
static_assert(Counter::is_always_lock_free, "Linux hardware metrics require lock-free 64-bit counters");
template <typename Enum> constexpr size_t Index(Enum value) { return static_cast<size_t>(value); }
void Add(Counter& counter, uint64_t value = 1) { counter.fetch_add(value, std::memory_order_relaxed); }
void Sub(Counter& counter, uint64_t value = 1) { counter.fetch_sub(value, std::memory_order_relaxed); }
uint64_t Read(const Counter& value) { return value.load(std::memory_order_relaxed); }
struct Duration {
  Counter samples{0}, total{0}, maximum{0};
  std::array<Counter, kLinuxEncoderDurationUpperBoundsNs.size()> buckets{};
  void Record(uint64_t ns) {
    Add(samples); Add(total, ns);
    auto previous = maximum.load(std::memory_order_relaxed);
    while (previous < ns && !maximum.compare_exchange_weak(previous, ns, std::memory_order_relaxed)) {}
    for (size_t i = 0; i < buckets.size(); ++i) {
      if (ns <= kLinuxEncoderDurationUpperBoundsNs[i]) { Add(buckets[i]); break; }
    }
  }
  LinuxEncoderDurationSnapshot Snapshot() const {
    LinuxEncoderDurationSnapshot value;
    value.samples = Read(samples); value.total_ns = Read(total); value.max_ns = Read(maximum);
    for (size_t i = 0; i < buckets.size(); ++i) value.buckets[i] = Read(buckets[i]);
    return value;
  }
};
struct Transfer {
  Counter started{0}, requested{0}, successful{0}, bytes{0}; Duration completed;
};
struct Operation { Counter started{0}, timeouts{0}; Duration queue, service; };
struct Pool {
  Counter allocations{0}, allocated_surfaces{0}, owned_pools{0}, owned_surfaces{0}, owned_bytes{0};
  Counter retained_pools{0}, retained_surfaces{0}, retained_bytes{0};
  Counter rebuild_started{0}, rebuild_succeeded{0}, rebuild_failed{0};
};
struct Workers {
  Counter capacity{0}, occupied{0}, running{0}, retiring{0}, retained{0};
  Counter started{0}, finished{0}, start_failures{0}, capacity_rejections{0};
};
struct Backend { Counter created{0}, open_attempts{0}, open_succeeded{0}; };
struct Metrics {
  std::array<Transfer, Index(LinuxEncoderMetricTransfer::kCount)> transfers;
  std::array<Operation, Index(LinuxEncoderMetricOperation::kCount)> operations;
  std::array<Pool, Index(LinuxEncoderMetricBackend::kCount)> pools;
  std::array<Backend, Index(LinuxEncoderMetricBackend::kCount)> backends;
  Workers workers;
};
Metrics& Sink() { static auto* const metrics = new Metrics; return *metrics; }
}  // namespace
uint64_t LinuxEncoderElapsedNs(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}
LinuxHardwareEncoderMetricsSnapshot SnapshotLinuxHardwareEncoderMetrics() {
  LinuxHardwareEncoderMetricsSnapshot result; const auto& metrics = Sink();
  for (size_t i = 0; i < result.transfers.size(); ++i) {
    const auto& t = metrics.transfers[i]; auto& out = result.transfers[i];
    out.measurement_available = i != Index(LinuxEncoderMetricTransfer::kVaapiUpload);
    out.started_calls = Read(t.started); out.requested_bytes = Read(t.requested);
    out.successful_calls = Read(t.successful); out.successful_bytes = Read(t.bytes); out.completed = t.completed.Snapshot();
  }
  for (size_t i = 0; i < result.operations.size(); ++i) {
    const auto& operation = metrics.operations[i]; auto& out = result.operations[i];
    out.started_calls = Read(operation.started); out.timeouts = Read(operation.timeouts);
    out.queue_wait = operation.queue.Snapshot(); out.service = operation.service.Snapshot();
  }
  for (size_t i = 0; i < result.pools.size(); ++i) {
    const auto& pool = metrics.pools[i]; auto& out = result.pools[i];
    out.measurement_available = i == Index(LinuxEncoderMetricBackend::kQsv);
    out.allocation_calls = Read(pool.allocations); out.allocated_surfaces = Read(pool.allocated_surfaces);
    out.owned_pools = Read(pool.owned_pools); out.owned_surfaces = Read(pool.owned_surfaces); out.owned_bytes = Read(pool.owned_bytes);
    out.retained_pools = Read(pool.retained_pools); out.retained_surfaces = Read(pool.retained_surfaces); out.retained_bytes = Read(pool.retained_bytes);
    out.rebuild_started = Read(pool.rebuild_started); out.rebuild_succeeded = Read(pool.rebuild_succeeded); out.rebuild_failed = Read(pool.rebuild_failed);
  }
  for (size_t i = 0; i < result.backends.size(); ++i) {
    result.backends[i].created_instances = Read(metrics.backends[i].created);
    result.backends[i].open_attempts = Read(metrics.backends[i].open_attempts);
    result.backends[i].open_succeeded = Read(metrics.backends[i].open_succeeded);
  }
  const auto& w = metrics.workers; auto& out = result.workers;
  out.capacity = Read(w.capacity); out.occupied_slots = Read(w.occupied); out.running = Read(w.running);
  out.retiring = Read(w.retiring); out.retained_slots = Read(w.retained); out.started = Read(w.started);
  out.finished = Read(w.finished); out.thread_start_failures = Read(w.start_failures); out.capacity_rejections = Read(w.capacity_rejections);
  return result;
}
LinuxEncoderTransferTimer::LinuxEncoderTransferTimer(LinuxEncoderMetricTransfer kind, uint64_t bytes)
    : kind_(kind), bytes_(bytes), start_(std::chrono::steady_clock::now()) {
  auto& transfer = Sink().transfers[Index(kind_)]; Add(transfer.started); Add(transfer.requested, bytes_);
}
LinuxEncoderTransferTimer::~LinuxEncoderTransferTimer() { if (!finished_) Finish(false); }
void LinuxEncoderTransferTimer::Finish(bool successful) {
  if (finished_) return;
  const auto ns = LinuxEncoderElapsedNs(start_); finished_ = true;
  auto& transfer = Sink().transfers[Index(kind_)]; transfer.completed.Record(ns);
  if (successful) { Add(transfer.successful); Add(transfer.bytes, bytes_); }
}
void RecordLinuxEncoderOperationStarted(LinuxEncoderMetricOperation kind, uint64_t queue_ns) {
  auto& operation = Sink().operations[Index(kind)]; Add(operation.started); operation.queue.Record(queue_ns);
}
void RecordLinuxEncoderOperationCompleted(LinuxEncoderMetricOperation kind, uint64_t service_ns) {
  Sink().operations[Index(kind)].service.Record(service_ns);
}
void RecordLinuxEncoderOperationTimeout(LinuxEncoderMetricOperation kind) { Add(Sink().operations[Index(kind)].timeouts); }
void SetLinuxEncoderWorkerCapacity(uint64_t capacity) { Sink().workers.capacity.store(capacity, std::memory_order_relaxed); }
void RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent event) {
  auto& workers = Sink().workers;
  switch (event) {
    case LinuxEncoderWorkerEvent::kReserved: Add(workers.occupied); break;
    case LinuxEncoderWorkerEvent::kReservationReleased: Sub(workers.occupied); break;
    case LinuxEncoderWorkerEvent::kStarted: Add(workers.running); Add(workers.started); break;
    case LinuxEncoderWorkerEvent::kFinished: Sub(workers.running); Add(workers.finished); break;
    case LinuxEncoderWorkerEvent::kRetiring: Add(workers.retiring); break;
    case LinuxEncoderWorkerEvent::kRetirementFinished: Sub(workers.retiring); break;
    case LinuxEncoderWorkerEvent::kRetained: Add(workers.retained); break;
    case LinuxEncoderWorkerEvent::kThreadStartFailed: Add(workers.start_failures); break;
    case LinuxEncoderWorkerEvent::kCapacityRejected: Add(workers.capacity_rejections); break;
  }
}
void RecordLinuxEncoderPoolAllocated(LinuxEncoderMetricBackend kind, uint64_t surfaces, uint64_t bytes) {
  auto& pool = Sink().pools[Index(kind)]; Add(pool.allocations); Add(pool.allocated_surfaces, surfaces);
  Add(pool.owned_pools); Add(pool.owned_surfaces, surfaces); Add(pool.owned_bytes, bytes);
}
void RecordLinuxEncoderPoolRetained(LinuxEncoderMetricBackend kind, uint64_t surfaces, uint64_t bytes) {
  auto& pool = Sink().pools[Index(kind)]; Add(pool.retained_pools); Add(pool.retained_surfaces, surfaces); Add(pool.retained_bytes, bytes);
}
void RecordLinuxEncoderPoolReleased(LinuxEncoderMetricBackend kind, uint64_t surfaces, uint64_t bytes, bool retained) {
  auto& pool = Sink().pools[Index(kind)]; Sub(pool.owned_pools); Sub(pool.owned_surfaces, surfaces); Sub(pool.owned_bytes, bytes);
  if (retained) { Sub(pool.retained_pools); Sub(pool.retained_surfaces, surfaces); Sub(pool.retained_bytes, bytes); }
}
void RecordLinuxEncoderBackendCreated(LinuxEncoderMetricBackend kind) {
  Add(Sink().backends[Index(kind)].created);
}
void RecordLinuxEncoderBackendOpenAttempt(LinuxEncoderMetricBackend kind) {
  Add(Sink().backends[Index(kind)].open_attempts);
}
void RecordLinuxEncoderBackendOpenSucceeded(LinuxEncoderMetricBackend kind) {
  Add(Sink().backends[Index(kind)].open_succeeded);
}
void RecordLinuxEncoderRebuildStarted(LinuxEncoderMetricBackend kind) { Add(Sink().pools[Index(kind)].rebuild_started); }
void RecordLinuxEncoderRebuildCompleted(LinuxEncoderMetricBackend kind, bool successful) {
  auto& pool = Sink().pools[Index(kind)]; if (successful) Add(pool.rebuild_succeeded); else Add(pool.rebuild_failed);
}
LinuxEncoderRebuildTimer::LinuxEncoderRebuildTimer(LinuxEncoderMetricBackend backend) : backend_(backend) { RecordLinuxEncoderRebuildStarted(backend_); }
LinuxEncoderRebuildTimer::~LinuxEncoderRebuildTimer() { RecordLinuxEncoderRebuildCompleted(backend_, successful_); }
}  // namespace webrtc::external_encoder
