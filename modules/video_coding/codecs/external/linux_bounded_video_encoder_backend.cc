/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"

#include <pthread.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace webrtc::external_encoder {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kWorkerCapacity = 8;
constexpr int kMaximumDimension = 8192;
std::atomic<bool>& LinuxRetirementFailure() {
  static auto* const failed = new std::atomic<bool>(false);
  return *failed;
}
enum class Operation { kOpen, kSetRates, kEncode, kClose, kShutdown };

const char* TimeoutReason(Operation operation) {
  switch (operation) {
    case Operation::kOpen: return "linux_backend_open_timeout";
    case Operation::kSetRates: return "linux_backend_set_rates_timeout";
    case Operation::kEncode: return "linux_backend_encode_timeout";
    case Operation::kClose: return "linux_backend_close_timeout";
    case Operation::kShutdown: return "linux_backend_shutdown_timeout";
  }
  return "linux_backend_timeout";
}

LinuxEncoderMetricOperation MetricOperation(Operation operation) {
  switch (operation) {
    case Operation::kOpen: return LinuxEncoderMetricOperation::kOpen;
    case Operation::kSetRates: return LinuxEncoderMetricOperation::kSetRates;
    case Operation::kEncode: return LinuxEncoderMetricOperation::kEncode;
    case Operation::kClose: return LinuxEncoderMetricOperation::kClose;
    case Operation::kShutdown: return LinuxEncoderMetricOperation::kShutdown;
  }
  return LinuxEncoderMetricOperation::kShutdown;
}

struct RetirementPolicy {
  RetirementPolicy() = default;
  explicit RetirementPolicy(HardwareEncoderVendor value) : vendor(value) {}
  explicit RetirementPolicy(LinuxBackendRetirementHooks value)
      : hooks(value), custom(true) {}
  bool Valid() const { return !custom || (hooks.check && hooks.record); }
  Status Check() const {
    if (CheckLinuxBackendRetirement() != Status::kOk) return Status::kRetirementFailed;
    return custom ? hooks.check() : HardwareEncoderProbes().Check(vendor);
  }
  void Record(std::string_view reason) const {
    RecordLinuxBackendRetirementFailure(reason);
    if (custom) hooks.record(reason);
    else HardwareEncoderProbes().RecordRetirementFailure(vendor, reason);
  }
  HardwareEncoderVendor vendor = HardwareEncoderVendor::kNvenc;
  LinuxBackendRetirementHooks hooks;
  bool custom = false;
};

struct WorkerState {
  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable completed;
  std::unique_ptr<VideoEncoderBackend> backend;
  RetirementPolicy policy;
  Operation operation = Operation::kClose;
  Config config;
  Rates rates;
  Frame frame;
  std::vector<uint8_t> storage;
  bool force_key_frame = false;
  bool pending = false;
  bool done = false;
  bool sealed = false;
  Status result = Status::kOk;
  Status close_status = Status::kOk;
  Packet packet;
  std::string error;
  Clock::time_point queued_at, completed_at;
  bool retiring_counted = false;
  bool worker_done = false;
  void MarkRetiringLocked() {
    if (!retiring_counted && !worker_done) {
      retiring_counted = true;
      RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kRetiring);
    }
  }
  void RetainStorage() {
    if (!retain_storage.exchange(true))
      RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kRetained);
  }
  std::atomic<bool> finished{false};
  // Failed retirement may still refer to the owned CPU frame even after the
  // inner destructor has preserved its native resources. Keep this slot too.
  std::atomic<bool> retain_storage{false};
};

void* WorkerMain(void* argument) {
  auto& state = *static_cast<WorkerState*>(argument);
  for (;;) {
    Operation operation;
    Config config;
    Rates rates;
    Frame frame;
    bool force_key_frame;
    Clock::time_point queued_at;
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.wake.wait(lock, [&] { return state.pending || state.sealed; });
      if (state.sealed) break;
      operation = state.operation;
      config = state.config;
      rates = state.rates;
      frame = state.frame;
      force_key_frame = state.force_key_frame;
      queued_at = state.queued_at;
    }
    const auto service_start = Clock::now();
    RecordLinuxEncoderOperationStarted(MetricOperation(operation),
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(service_start - queued_at).count()));
    // No metadata mutex is held across a vendor call, including destruction.
    Status result = Status::kOk;
    Status close_status = Status::kOk;
    Packet packet;
    std::string error;
    switch (operation) {
      case Operation::kOpen:
        result = state.backend->Open(config);
        if (result != Status::kOk) {
          error.assign(state.backend->LastError());
          state.backend->Close();
          close_status = state.backend->LastCloseStatus();
        }
        break;
      case Operation::kSetRates:
        result = state.backend->SetRates(rates);
        break;
      case Operation::kEncode:
        result = state.backend->Encode(frame, force_key_frame, &packet);
        break;
      case Operation::kClose:
      case Operation::kShutdown:
        state.backend->Close();
        close_status = state.backend->LastCloseStatus();
        result = close_status;
        break;
    }
    if (error.empty() || close_status != Status::kOk)
      error.assign(state.backend->LastError());
    if (close_status != Status::kOk) result = Status::kRetirementFailed;
    if (result == Status::kRetirementFailed) {
      state.policy.Record("linux_backend_retirement_failed");
    }
    if (operation == Operation::kShutdown) {
      if (close_status != Status::kOk) state.RetainStorage();
      state.backend.reset();
    }
    const auto completed_at = Clock::now();
    RecordLinuxEncoderOperationCompleted(MetricOperation(operation),
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(completed_at - service_start).count()));
    bool finish;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (!state.sealed) {
        state.result = result;
        state.close_status = close_status;
        state.error = std::move(error);
        state.packet = std::move(packet);
        state.completed_at = completed_at;
        state.done = true;
        state.pending = false;
        if (result == Status::kRetirementFailed) {
          state.sealed = true;
          state.MarkRetiringLocked();
        }
      }
      finish = state.sealed || operation == Operation::kShutdown;
    }
    state.completed.notify_one();
    if (finish) break;
  }
  if (state.backend) {
    // A timed-out task still owns its driver and frame. Only this owner may
    // clean up after that call returns, however late that may be.
    state.backend->Close();
    if (state.backend->LastCloseStatus() != Status::kOk) {
      state.RetainStorage();
      state.policy.Record("linux_backend_late_retirement_failed");
    }
    state.backend.reset();
  }
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.worker_done = true;
    if (state.retiring_counted)
      RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kRetirementFinished);
    RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kFinished);
  }
  state.finished.store(true, std::memory_order_release);
  return nullptr;
}

class WorkerRegistry {
 public:
  WorkerRegistry() { SetLinuxEncoderWorkerCapacity(kWorkerCapacity); }
  bool Start(const std::shared_ptr<WorkerState>& state, bool fail_creation,
             std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Reap();
    for (auto& slot : slots_) {
      if (slot.state) continue;
      // Reserve BEFORE pthread_create, counting normal and retiring workers.
      slot.state = state;
      RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kReserved);
      if (fail_creation ||
          pthread_create(&slot.thread, nullptr, WorkerMain, state.get()) != 0) {
        slot.state.reset();
        RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kReservationReleased);
        RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kThreadStartFailed);
        *error = "linux_backend_thread_creation_failed";
        return false;
      }
      slot.joined = false;
      RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kStarted);
      return true;
    }
    RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kCapacityRejected);
    *error = "linux_backend_worker_capacity_exhausted";
    return false;
  }
  size_t Count() {
    std::lock_guard<std::mutex> lock(mutex_);
    Reap();
    size_t count = 0;
    for (const auto& slot : slots_) count += !!slot.state;
    return count;
  }
 private:
  void Reap() {
    for (auto& slot : slots_) {
      if (!slot.state || slot.joined ||
          !slot.state->finished.load(std::memory_order_acquire)) continue;
      // finished is set just before pthread exit; tryjoin can still report
      // EBUSY. Retain ownership until a later nonblocking reap succeeds.
      if (pthread_tryjoin_np(slot.thread, nullptr) != 0) continue;
      slot.joined = true;
      if (!slot.state->retain_storage.load()) {
        slot.state.reset();
        RecordLinuxEncoderWorkerEvent(LinuxEncoderWorkerEvent::kReservationReleased);
      }
    }
  }
  struct Slot {
    std::shared_ptr<WorkerState> state;
    pthread_t thread{};
    bool joined = false;
  };
  std::mutex mutex_;
  std::array<Slot, kWorkerCapacity> slots_;
};

WorkerRegistry& Registry() {
  // Also retains joinable threads and their resources during process teardown.
  static auto* const registry = new WorkerRegistry;
  return *registry;
}

bool ValidDeadlines(const LinuxBackendDeadlines& deadlines) {
  return deadlines.open.count() > 0 && deadlines.set_rates.count() > 0 &&
         deadlines.encode.count() > 0 && deadlines.close.count() > 0;
}

bool ValidPlane(int stride, int width, int height) {
  if (stride < width) return false;
  const size_t limit = static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max());
  return static_cast<size_t>(height - 1) <=
         (limit - static_cast<size_t>(width)) / static_cast<size_t>(stride);
}

class LinuxBoundedBackend final : public VideoEncoderBackend {
 public:
  LinuxBoundedBackend(std::unique_ptr<VideoEncoderBackend> backend,
                      RetirementPolicy policy, Info info,
                      LinuxBackendDeadlines deadlines, bool fail_creation)
      : policy_(policy), info_(std::move(info)), deadlines_(deadlines) {
    if (!backend || !policy.Valid() || !ValidDeadlines(deadlines)) {
      initial_status_ = Status::kInvalidArgument;
      error_ = "invalid_linux_backend_arguments";
      return;
    }
    if (policy_.Check() != Status::kOk) {
      sealed_ = true;
      close_status_ = Status::kRetirementFailed;
      error_ = "linux_backend_vendor_poisoned";
      return;
    }
    auto state = std::make_shared<WorkerState>();
    state->backend = std::move(backend);
    state->policy = policy_;
    if (!Registry().Start(state, fail_creation, &error_)) {
      // No driver call has started. Destroying this inert object is safe.
      initial_status_ = Status::kUnavailable;
      return;
    }
    state_ = std::move(state);
  }
  ~LinuxBoundedBackend() override {
    if (state_ && !sealed_) Run(Operation::kShutdown, deadlines_.close);
    Registry().Count();  // Never waits for a still-running thread.
  }
  Info GetInfo() const override { return info_; }
  Status Open(const Config& config) override {
    const auto status = Run(Operation::kOpen, deadlines_.open, &config);
    opened_ = status == Status::kOk;
    if (opened_) config_ = config;
    return status;
  }
  Status SetRates(const Rates& rates) override {
    return Run(Operation::kSetRates, deadlines_.set_rates, nullptr, &rates);
  }
  Status Encode(const Frame& frame, bool force_key_frame, Packet* packet) override {
    if (packet) *packet = Packet{};
    if (sealed_) return Status::kRetirementFailed;
    if (!state_) return initial_status_;
    if (!packet || !opened_ || !frame.y || !frame.uv || frame.width <= 0 ||
        frame.height <= 0 || (frame.width & 1) || (frame.height & 1) ||
        frame.width > kMaximumDimension || frame.height > kMaximumDimension ||
        frame.width != config_.width || frame.height != config_.height ||
        !ValidPlane(frame.stride_y, frame.width, frame.height) ||
        !ValidPlane(frame.stride_uv, frame.width, frame.height / 2)) {
      error_ = "invalid_linux_backend_frame";
      return Status::kInvalidArgument;
    }
    return Run(Operation::kEncode, deadlines_.encode, nullptr, nullptr, &frame,
               force_key_frame, packet);
  }
  void Close() override {
    opened_ = false;
    if (sealed_) return;
    if (!state_) return;  // Startup failure never entered a driver.
    const auto status = Run(Operation::kClose, deadlines_.close);
    if (status == Status::kRetirementFailed) close_status_ = status;
  }
  Status LastCloseStatus() const override { return close_status_; }
  // The worker never mutates this caller-owned snapshot.
  std::string_view LastError() const override { return error_; }

 private:
  Status SealLocked(const char* reason) {
    sealed_ = true;
    opened_ = false;
    close_status_ = Status::kRetirementFailed;
    error_ = reason;
    state_->sealed = true;
    state_->MarkRetiringLocked();
    state_->packet = Packet{};
    state_->wake.notify_one();
    policy_.Record(reason);
    return Status::kRetirementFailed;
  }
  Status Run(Operation operation, std::chrono::milliseconds budget,
             const Config* config = nullptr, const Rates* rates = nullptr,
             const Frame* source = nullptr, bool force_key_frame = false,
             Packet* packet = nullptr) {
    if (sealed_) return Status::kRetirementFailed;
    if (!state_) return initial_status_;
    const auto deadline = Clock::now() + budget;
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (policy_.Check() != Status::kOk) {
      return SealLocked("linux_backend_vendor_poisoned");
    }
    if (state_->sealed) return SealLocked("linux_backend_retirement_failed");
    state_->operation = operation;
    if (config) state_->config = *config;
    if (rates) state_->rates = *rates;
    if (source) {
      // Bounded dimensions were checked above. Dense owned NV12 storage is
      // reused; caller padding and borrowed pointers never reach the worker.
      const size_t y_size = static_cast<size_t>(source->width) * source->height;
      const size_t uv_size = y_size / 2;
      if (y_size > std::numeric_limits<size_t>::max() - uv_size) {
        error_ = "linux_backend_frame_size_overflow";
        return Status::kInvalidArgument;
      }
      state_->storage.resize(y_size + uv_size);
      LinuxEncoderTransferTimer staging(LinuxEncoderMetricTransfer::kOwnedNv12Staging, y_size + uv_size);
      for (int row = 0; row < source->height; ++row) {
        std::memcpy(state_->storage.data() + static_cast<size_t>(row) * source->width,
                    source->y + static_cast<size_t>(row) * source->stride_y,
                    source->width);
      }
      for (int row = 0; row < source->height / 2; ++row) {
        std::memcpy(state_->storage.data() + y_size +
                        static_cast<size_t>(row) * source->width,
                    source->uv + static_cast<size_t>(row) * source->stride_uv,
                    source->width);
      }
      state_->frame = {state_->storage.data(), state_->storage.data() + y_size,
                       source->width, source->width, source->width,
                       source->height, source->id};
      staging.Finish(true);
    }
    state_->force_key_frame = force_key_frame;
    state_->packet = Packet{};
    state_->done = false;
    state_->pending = true;
    state_->queued_at = Clock::now();
    state_->wake.notify_one();
    state_->completed.wait_until(lock, deadline,
                                [&] { return state_->done || state_->sealed; });
    if (state_->sealed) return SealLocked("linux_backend_retirement_failed");
    if (!state_->done || state_->completed_at > deadline) {
      RecordLinuxEncoderOperationTimeout(MetricOperation(operation));
      return SealLocked(TimeoutReason(operation));
    }
    // Another instance may have poisoned the vendor while this task ran.
    if (policy_.Check() != Status::kOk) {
      return SealLocked("linux_backend_vendor_poisoned");
    }
    error_ = state_->error;
    close_status_ = state_->close_status;
    const auto status = state_->result;
    if (packet && status == Status::kOk) *packet = std::move(state_->packet);
    return status;
  }
  RetirementPolicy policy_;
  const Info info_;
  LinuxBackendDeadlines deadlines_;
  std::shared_ptr<WorkerState> state_;
  Config config_;
  bool opened_ = false;
  bool sealed_ = false;
  Status initial_status_ = Status::kOk;
  Status close_status_ = Status::kOk;
  std::string error_;
};
}  // namespace

Status CheckLinuxBackendRetirement() {
  if (LinuxRetirementFailure().load(std::memory_order_acquire))
    return Status::kRetirementFailed;
  return HardwareEncoderProbes().CheckAny();
}
void RecordLinuxBackendRetirementFailure(std::string_view) {
  LinuxRetirementFailure().store(true, std::memory_order_release);
}

std::unique_ptr<VideoEncoderBackend> CreateLinuxBoundedBackend(
    std::unique_ptr<VideoEncoderBackend> backend, HardwareEncoderVendor vendor,
    Info cached_info, LinuxBackendDeadlines deadlines) {
  return std::make_unique<LinuxBoundedBackend>(
      std::move(backend), RetirementPolicy(vendor), std::move(cached_info), deadlines, false);
}

std::unique_ptr<VideoEncoderBackend> CreateLinuxBoundedBackendWithRetirementHooks(
    std::unique_ptr<VideoEncoderBackend> backend, Info cached_info,
    LinuxBackendRetirementHooks hooks, LinuxBackendDeadlines deadlines) {
  return std::make_unique<LinuxBoundedBackend>(
      std::move(backend), RetirementPolicy(hooks), std::move(cached_info), deadlines, false);
}

namespace internal {
std::unique_ptr<VideoEncoderBackend> CreateLinuxBoundedBackendForTesting(
    std::unique_ptr<VideoEncoderBackend> backend, HardwareEncoderVendor vendor,
    Info cached_info, LinuxBackendDeadlines deadlines,
    bool fail_thread_creation) {
  return std::make_unique<LinuxBoundedBackend>(std::move(backend), RetirementPolicy(vendor),
      std::move(cached_info), deadlines, fail_thread_creation);
}
size_t LinuxBoundedWorkerCountForTesting() { return Registry().Count(); }
size_t LinuxBoundedWorkerCapacityForTesting() { return kWorkerCapacity; }
}  // namespace internal
}  // namespace webrtc::external_encoder
