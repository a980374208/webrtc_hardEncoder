/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace webrtc::external_encoder {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void Require(bool value) { if (!value) std::abort(); }
enum class Call { kNone, kOpen, kRates, kEncode, kClose, kDestroy };
struct Control {
  std::mutex mutex;
  std::condition_variable cv;
  Call block = Call::kNone;
  bool entered = false;
  bool released = false;
  bool have_owner = false;
  pthread_t owner{};
  std::atomic<int> opened{0}, encoded{0}, rated{0}, closed{0}, destroyed{0};
  Status open_result = Status::kOk;
  Status encode_result = Status::kOk;
  Status close_result = Status::kOk;
  Rates last_rates;
  bool saw_owned_frame = false;
  std::vector<uint8_t> observed;
  uint64_t observed_id = 0;
  void Owner() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!have_owner) { owner = pthread_self(); have_owner = true; }
    Require(pthread_equal(owner, pthread_self()));
  }
  void Block(Call call) {
    std::unique_lock<std::mutex> lock(mutex);
    if (block != call) return;
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }
  void WaitEntered() {
    std::unique_lock<std::mutex> lock(mutex);
    Require(cv.wait_for(lock, 2s, [&] { return entered; }));
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    cv.notify_all();
  }
};
class FakeBackend final : public VideoEncoderBackend {
 public:
  explicit FakeBackend(std::shared_ptr<Control> control)
      : control_(std::move(control)) {}
  ~FakeBackend() override {
    // Failed pthread creation is the only case destroyed before a worker owns
    // this inert object. Every object with any method call has one owner.
    if (control_->have_owner) control_->Owner();
    control_->Block(Call::kDestroy);
    ++control_->destroyed;
  }
  Info GetInfo() const override { std::abort(); }  // Must use cached Info.
  Status Open(const Config&) override {
    control_->Owner(); ++control_->opened;
    control_->Block(Call::kOpen);
    error_ = control_->open_result == Status::kOk ? "" : "fake_open_failed";
    return control_->open_result;
  }
  Status SetRates(const Rates& rates) override {
    control_->Owner(); ++control_->rated;
    control_->Block(Call::kRates);
    control_->last_rates = rates;
    return Status::kOk;
  }
  Status Encode(const Frame& frame, bool force, Packet* packet) override {
    control_->Owner(); ++control_->encoded;
    control_->Block(Call::kEncode);
    control_->saw_owned_frame = frame.stride_y == frame.width &&
                                frame.stride_uv == frame.width;
    control_->observed.clear();
    for (int row = 0; row < frame.height; ++row)
      control_->observed.insert(control_->observed.end(),
          frame.y + row * frame.stride_y, frame.y + row * frame.stride_y + frame.width);
    for (int row = 0; row < frame.height / 2; ++row)
      control_->observed.insert(control_->observed.end(),
          frame.uv + row * frame.stride_uv, frame.uv + row * frame.stride_uv + frame.width);
    control_->observed_id = frame.id;
    packet->bytes = {1, 2, 3}; packet->frame_id = frame.id;
    packet->key_frame = force; packet->qp = 17;
    error_ = control_->encode_result == Status::kOk ? "" : "fake_encode_failed";
    return control_->encode_result;
  }
  void Close() override {
    control_->Owner(); ++control_->closed;
    control_->Block(Call::kClose);
    error_ = "fake_closed";
  }
  Status LastCloseStatus() const override {
    control_->Owner(); return control_->close_result;
  }
  std::string_view LastError() const override {
    control_->Owner(); return error_;
  }
 private:
  std::shared_ptr<Control> control_;
  std::string error_;
};
LinuxBackendDeadlines Short() { return {40ms, 40ms, 40ms, 40ms}; }
Config Configuration() {
  Config result; result.width = 4; result.height = 4;
  result.bitrate_bps = 100000; result.framerate_fps = 30; return result;
}
std::unique_ptr<VideoEncoderBackend> Backend(
    const std::shared_ptr<Control>& control,
    HardwareEncoderVendor vendor = HardwareEncoderVendor::kNvenc,
    LinuxBackendDeadlines deadlines = Short()) {
  return CreateLinuxBoundedBackend(std::make_unique<FakeBackend>(control), vendor,
                                  {"cached-linux-hardware", true}, deadlines);
}
struct Input {
  std::vector<uint8_t> y = std::vector<uint8_t>(8 * 4, 0x21);
  std::vector<uint8_t> uv = std::vector<uint8_t>(8 * 2, 0x89);
  Frame frame{y.data(), uv.data(), 8, 8, 4, 4, 123};
  std::vector<uint8_t> Expected() const {
    std::vector<uint8_t> value(16, 0x21); value.resize(24, 0x89); return value;
  }
};
void Bounded(Clock::time_point start) { Require(Clock::now() - start < 1s); }
void Reaped(size_t expected = 0) {
  const auto deadline = Clock::now() + 2s;
  while (internal::LinuxBoundedWorkerCountForTesting() != expected) {
    Require(Clock::now() < deadline); usleep(1000);
  }
}
void Destroyed(const std::shared_ptr<Control>& control) {
  const auto deadline = Clock::now() + 2s;
  while (control->destroyed.load() == 0) {
    Require(Clock::now() < deadline); usleep(1000);
  }
}
void Poisoned(VideoEncoderBackend& backend) {
  Require(backend.LastCloseStatus() == Status::kRetirementFailed);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc) ==
          Status::kRetirementFailed);
  Require(backend.Open(Configuration()) == Status::kRetirementFailed);
  Require(backend.SetRates({10000, 10}) == Status::kRetirementFailed);
  backend.Close();
  Require(backend.LastCloseStatus() == Status::kRetirementFailed);
}
template <typename Enum> constexpr size_t MetricIndex(Enum kind) {
  return static_cast<size_t>(kind);
}
void HistogramValid(const LinuxEncoderDurationSnapshot& duration, uint64_t count) {
  uint64_t total = 0;
  for (const auto bucket : duration.buckets) total += bucket;
  Require(duration.samples == count && total == count &&
      duration.total_ns >= duration.max_ns);
}
void Happy() {
  auto control = std::make_shared<Control>(); auto backend = Backend(control);
  const auto info = backend->GetInfo();
  Require(info.implementation_name == "cached-linux-hardware" && info.is_hardware_accelerated);
  backend->Close(); Require(backend->LastCloseStatus() == Status::kOk);
  Require(backend->Open(Configuration()) == Status::kOk);
  Require(backend->SetRates({90000, 15}) == Status::kOk);
  Require(control->last_rates.bitrate_bps == 90000 && control->last_rates.framerate_fps == 15);
  Input input; Packet first, second;
  Require(backend->Encode(input.frame, true, &first) == Status::kOk);
  Require(first.bytes == std::vector<uint8_t>({1, 2, 3}) && first.frame_id == 123 && first.key_frame && first.qp == 17);
  Require(control->observed == input.Expected() && control->saw_owned_frame);
  input.frame.id = 124;
  Require(backend->Encode(input.frame, false, &second) == Status::kOk && second.frame_id == 124 && !second.key_frame);
  second.bytes[0] = 7; Require(first.bytes[0] == 1);
  backend->Close(); Require(backend->LastCloseStatus() == Status::kOk);
  Require(backend->Open(Configuration()) == Status::kOk);
  backend.reset(); Destroyed(control); Reaped();
  Require(control->opened == 2 && control->encoded == 2 && control->rated == 1);
  Require(!pthread_equal(control->owner, pthread_self()));
  const auto metrics = SnapshotLinuxHardwareEncoderMetrics();
  const auto& staging = metrics.transfers[MetricIndex(LinuxEncoderMetricTransfer::kOwnedNv12Staging)];
  Require(metrics.schema_version == 1 && metrics.scope == LinuxEncoderMetricsScope::kProcess);
  Require(staging.measurement_available && staging.started_calls == 2 && staging.successful_calls == 2);
  Require(staging.requested_bytes == 48 && staging.successful_bytes == 48);
  HistogramValid(staging.completed, 2);
  const auto& encode = metrics.operations[MetricIndex(LinuxEncoderMetricOperation::kEncode)];
  Require(encode.started_calls == 2 && encode.timeouts == 0);
  HistogramValid(encode.queue_wait, 2); HistogramValid(encode.service, 2);
  Require(metrics.workers.running == 0 && metrics.workers.retiring == 0 &&
      metrics.workers.occupied_slots == 0 && metrics.workers.retained_slots == 0);
  Require(metrics.workers.started == 1 && metrics.workers.finished == 1 && metrics.workers.capacity == 8);
  Require(!metrics.gpu_readback_bytes.has_value());
  Require(!metrics.transfers[MetricIndex(LinuxEncoderMetricTransfer::kVaapiUpload)].measurement_available);
}
void InvalidFrames() {
  auto control = std::make_shared<Control>(); auto backend = Backend(control);
  Require(backend->Open(Configuration()) == Status::kOk);
  Input input; Packet packet;
  auto invalid = input.frame; invalid.stride_y = 3;
  Require(backend->Encode(invalid, false, &packet) == Status::kInvalidArgument);
  invalid = input.frame; invalid.stride_uv = -1;
  Require(backend->Encode(invalid, false, &packet) == Status::kInvalidArgument);
  invalid = input.frame; invalid.y = nullptr;
  Require(backend->Encode(invalid, false, &packet) == Status::kInvalidArgument);
  invalid = input.frame; invalid.width = 3;
  Require(backend->Encode(invalid, false, &packet) == Status::kInvalidArgument);
  invalid = input.frame; invalid.height = 8194;
  Require(backend->Encode(invalid, false, &packet) == Status::kInvalidArgument);
  Require(backend->Encode(input.frame, false, nullptr) == Status::kInvalidArgument);
  Require(control->encoded == 0); backend.reset(); Reaped();
}
void OrdinaryOpenFailure() {
  auto control = std::make_shared<Control>(); control->open_result = Status::kUnavailable;
  auto backend = Backend(control);
  Require(backend->Open(Configuration()) == Status::kUnavailable);
  Require(backend->LastError() == "fake_open_failed" && control->closed == 1);
  Require(backend->LastCloseStatus() == Status::kOk);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc) == Status::kOk);
  control->open_result = Status::kOk;
  Require(backend->Open(Configuration()) == Status::kOk);
  backend.reset(); Reaped();
}
void OrdinaryEncodeFailure() {
  auto control = std::make_shared<Control>(); control->encode_result = Status::kError;
  auto backend = Backend(control); Require(backend->Open(Configuration()) == Status::kOk);
  Input input; Packet packet;
  Require(backend->Encode(input.frame, false, &packet) == Status::kError && packet.bytes.empty());
  Require(backend->LastError() == "fake_encode_failed");
  backend->Close(); Require(backend->LastCloseStatus() == Status::kOk);
  backend.reset(); Reaped();
}
void OpenTimeout() {
  auto control = std::make_shared<Control>(); control->block = Call::kOpen;
  auto backend = Backend(control); const auto start = Clock::now();
  Require(backend->Open(Configuration()) == Status::kRetirementFailed); Bounded(start);
  control->WaitEntered(); Require(backend->LastError() == "linux_backend_open_timeout");
  const std::string_view snapshot = backend->LastError(); Poisoned(*backend);
  control->Release(); Destroyed(control); Reaped();
  Require(snapshot == "linux_backend_open_timeout");
  Require(backend->LastError() == "linux_backend_open_timeout");
  Poisoned(*backend); backend.reset();
}
void EncodeTimeoutOwnedInput() {
  auto control = std::make_shared<Control>(); control->block = Call::kEncode;
  auto backend = Backend(control); Require(backend->Open(Configuration()) == Status::kOk);
  Input input; const auto expected = input.Expected(); Packet packet; packet.bytes = {9};
  const auto start = Clock::now();
  Require(backend->Encode(input.frame, true, &packet) == Status::kRetirementFailed); Bounded(start);
  control->WaitEntered(); Require(packet.bytes.empty());
  const auto active = SnapshotLinuxHardwareEncoderMetrics();
  const auto& encode = active.operations[MetricIndex(LinuxEncoderMetricOperation::kEncode)];
  Require(encode.started_calls == 1 && encode.timeouts == 1 && encode.service.samples == 0);
  Require(active.workers.running == 1 && active.workers.retiring == 1 && active.workers.occupied_slots == 1);
  Require(backend->LastError() == "linux_backend_encode_timeout"); Poisoned(*backend);
  input.y.clear(); input.y.shrink_to_fit(); input.uv.clear(); input.uv.shrink_to_fit();
  const auto destroy_start = Clock::now(); backend.reset(); Bounded(destroy_start);
  Require(control->destroyed == 0);
  control->Release(); Destroyed(control); Reaped();
  Require(control->observed == expected && control->observed_id == 123 && control->saw_owned_frame);
  Require(packet.bytes.empty());  // No late output and no caller Packet* retained.
  const auto retired = SnapshotLinuxHardwareEncoderMetrics();
  Require(retired.workers.running == 0 && retired.workers.retiring == 0 && retired.workers.occupied_slots == 0);
  HistogramValid(retired.operations[MetricIndex(LinuxEncoderMetricOperation::kEncode)].service, 1);
}
void RatesTimeout() {
  auto control = std::make_shared<Control>(); control->block = Call::kRates;
  auto backend = Backend(control); Require(backend->Open(Configuration()) == Status::kOk);
  const auto start = Clock::now();
  Require(backend->SetRates({90000, 15}) == Status::kRetirementFailed); Bounded(start);
  control->WaitEntered(); Require(backend->LastError() == "linux_backend_set_rates_timeout");
  Poisoned(*backend); backend.reset(); control->Release(); Destroyed(control); Reaped();
}
void CloseTimeout() {
  auto control = std::make_shared<Control>(); auto backend = Backend(control);
  Require(backend->Open(Configuration()) == Status::kOk); control->block = Call::kClose;
  const auto start = Clock::now(); backend->Close(); Bounded(start);
  control->WaitEntered(); Require(backend->LastError() == "linux_backend_close_timeout");
  Poisoned(*backend); backend.reset(); control->Release(); Destroyed(control); Reaped();
}
void DestructorTimeout() {
  auto control = std::make_shared<Control>(); auto backend = Backend(control);
  Require(backend->Open(Configuration()) == Status::kOk); control->block = Call::kClose;
  const auto start = Clock::now(); backend.reset(); Bounded(start);
  control->WaitEntered(); Require(control->destroyed == 0);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc) == Status::kRetirementFailed);
  control->Release(); Destroyed(control); Reaped();
}
void InnerDestructorTimeout() {
  auto control = std::make_shared<Control>(); auto backend = Backend(control);
  Require(backend->Open(Configuration()) == Status::kOk); control->block = Call::kDestroy;
  const auto start = Clock::now(); backend.reset(); Bounded(start);
  control->WaitEntered(); Require(control->destroyed == 0);
  control->Release(); Destroyed(control); Reaped();
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc) == Status::kRetirementFailed);
}
void CloseFailure() {
  auto control = std::make_shared<Control>(); auto backend = Backend(control);
  Require(backend->Open(Configuration()) == Status::kOk);
  control->close_result = Status::kRetirementFailed;
  backend->Close(); Poisoned(*backend); backend.reset(); Destroyed(control); Reaped(1);
  const auto deadline = Clock::now() + 2s;
  while (SnapshotLinuxHardwareEncoderMetrics().workers.running != 0) {
    Require(Clock::now() < deadline); usleep(1000);
  }
  const auto metrics = SnapshotLinuxHardwareEncoderMetrics();
  Require(metrics.workers.occupied_slots == 1 && metrics.workers.retained_slots == 1 &&
      metrics.workers.retiring == 0 && metrics.workers.running == 0);
}
void LateCleanupFailure() {
  auto control = std::make_shared<Control>(); control->block = Call::kEncode;
  auto backend = Backend(control); Require(backend->Open(Configuration()) == Status::kOk);
  Input input; Packet packet;
  Require(backend->Encode(input.frame, false, &packet) == Status::kRetirementFailed);
  control->WaitEntered(); control->close_result = Status::kRetirementFailed;
  backend.reset(); control->Release(); Destroyed(control); Reaped(1);
}
void ThreadFailure() {
  auto control = std::make_shared<Control>();
  auto backend = internal::CreateLinuxBoundedBackendForTesting(
      std::make_unique<FakeBackend>(control), HardwareEncoderVendor::kNvenc,
      {"cached", true}, Short(), true);
  Require(backend->Open(Configuration()) == Status::kUnavailable);
  Require(backend->LastError() == "linux_backend_thread_creation_failed");
  backend->Close(); Require(backend->LastCloseStatus() == Status::kOk);
  Require(control->opened == 0 && control->closed == 0 && control->destroyed == 1);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc) == Status::kOk);
  backend.reset(); Reaped();
}
void CapacityActive() {
  std::vector<std::unique_ptr<VideoEncoderBackend>> backends;
  const size_t capacity = internal::LinuxBoundedWorkerCapacityForTesting();
  for (size_t i = 0; i < capacity; ++i) backends.push_back(Backend(std::make_shared<Control>()));
  Require(internal::LinuxBoundedWorkerCountForTesting() == capacity);
  auto overflow = std::make_shared<Control>(); auto backend = Backend(overflow);
  Require(backend->Open(Configuration()) == Status::kUnavailable);
  Require(backend->LastError() == "linux_backend_worker_capacity_exhausted");
  Require(overflow->opened == 0 && overflow->closed == 0);
  backends.clear(); backend.reset(); Reaped();
  backend = Backend(std::make_shared<Control>());
  Require(backend->Open(Configuration()) == Status::kOk); backend.reset(); Reaped();
}
void CapacityIncludesRetired() {
  auto control = std::make_shared<Control>(); control->block = Call::kOpen;
  auto retiring = Backend(control);
  const size_t capacity = internal::LinuxBoundedWorkerCapacityForTesting();
  std::vector<std::unique_ptr<VideoEncoderBackend>> backends;
  for (size_t i = 1; i < capacity; ++i)
    backends.push_back(Backend(std::make_shared<Control>(), HardwareEncoderVendor::kQsv));
  auto overflow = Backend(std::make_shared<Control>(), HardwareEncoderVendor::kAmf);
  Require(overflow->Open(Configuration()) == Status::kUnavailable);
  Require(retiring->Open(Configuration()) == Status::kRetirementFailed);
  control->WaitEntered(); retiring.reset();
  Require(internal::LinuxBoundedWorkerCountForTesting() == capacity);
  backends.clear(); Reaped(1);  // The timed-out worker still occupies its slot.
  control->Release(); Destroyed(control); Reaped();
}
void PoisonBeforeCall() {
  auto slow = std::make_shared<Control>(); slow->block = Call::kOpen;
  auto first = Backend(slow); auto other = std::make_shared<Control>(); auto second = Backend(other);
  Require(first->Open(Configuration()) == Status::kRetirementFailed); slow->WaitEntered();
  Require(second->Open(Configuration()) == Status::kRetirementFailed && other->opened == 0);
  auto third_control = std::make_shared<Control>(); auto third = Backend(third_control);
  Require(third->Open(Configuration()) == Status::kRetirementFailed && third_control->opened == 0);
  first.reset(); second.reset(); third.reset(); slow->Release(); Destroyed(slow); Reaped();
}
struct EncodeCaller { VideoEncoderBackend* backend; Status result; Packet packet; };
void* EncodeOnThread(void* argument) {
  auto& caller = *static_cast<EncodeCaller*>(argument); Input input;
  caller.result = caller.backend->Encode(input.frame, false, &caller.packet); return nullptr;
}
void PoisonDuringCall() {
  auto slow = std::make_shared<Control>(); slow->block = Call::kOpen;
  auto first = Backend(slow);
  auto active = std::make_shared<Control>(); active->block = Call::kEncode;
  auto second = Backend(active, HardwareEncoderVendor::kNvenc, {2s, 2s, 2s, 2s});
  Require(second->Open(Configuration()) == Status::kOk);
  EncodeCaller caller{second.get(), Status::kOk, {}}; pthread_t thread;
  Require(pthread_create(&thread, nullptr, EncodeOnThread, &caller) == 0); active->WaitEntered();
  Require(first->Open(Configuration()) == Status::kRetirementFailed); slow->WaitEntered();
  active->Release(); Require(pthread_join(thread, nullptr) == 0);
  Require(caller.result == Status::kRetirementFailed && caller.packet.bytes.empty());
  Poisoned(*second); first.reset(); second.reset(); slow->Release(); Destroyed(slow); Reaped();
}
std::atomic<bool> hook_failed{false};
std::atomic<int> hook_records{0};
Status CheckHook() { return hook_failed ? Status::kRetirementFailed : Status::kOk; }
void RecordHook(std::string_view reason) {
  Require(!reason.empty()); hook_failed = true; ++hook_records;
}
void IndependentHooksTimeout() {
  auto control = std::make_shared<Control>(); control->block = Call::kEncode;
  auto backend = CreateLinuxBoundedBackendWithRetirementHooks(
      std::make_unique<FakeBackend>(control), {"independent", true},
      {CheckHook, RecordHook}, Short());
  auto other_control = std::make_shared<Control>();
  auto other = CreateLinuxBoundedBackendWithRetirementHooks(
      std::make_unique<FakeBackend>(other_control), {"independent", true},
      {CheckHook, RecordHook}, Short());
  Require(backend->Open(Configuration()) == Status::kOk);
  Input input; Packet packet;
  Require(backend->Encode(input.frame, false, &packet) == Status::kRetirementFailed);
  control->WaitEntered(); Require(hook_records > 0 && CheckHook() == Status::kRetirementFailed);
  Require(backend->LastCloseStatus() == Status::kRetirementFailed);
  Require(other->Open(Configuration()) == Status::kRetirementFailed && other_control->opened == 0);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc) == Status::kOk);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kAmf) == Status::kOk);
  control->Release(); Destroyed(control); Reaped();
  Require(packet.bytes.empty() && backend->SetRates({1000, 1}) == Status::kRetirementFailed);
  backend.reset(); other.reset();
}
void CrossVendorPoisonDuringCall() {
  auto slow = std::make_shared<Control>(); slow->block = Call::kOpen;
  auto first = Backend(slow);
  auto active = std::make_shared<Control>(); active->block = Call::kEncode;
  auto second = Backend(active, HardwareEncoderVendor::kQsv, {2s, 2s, 2s, 2s});
  Require(second->Open(Configuration()) == Status::kOk);
  EncodeCaller caller{second.get(), Status::kOk, {}}; pthread_t thread;
  Require(pthread_create(&thread, nullptr, EncodeOnThread, &caller) == 0); active->WaitEntered();
  Require(first->Open(Configuration()) == Status::kRetirementFailed); slow->WaitEntered();
  Require(CheckLinuxBackendRetirement() == Status::kRetirementFailed);
  active->Release(); Require(pthread_join(thread, nullptr) == 0);
  Require(caller.result == Status::kRetirementFailed && caller.packet.bytes.empty());
  Require(second->LastCloseStatus() == Status::kRetirementFailed);
  first.reset(); second.reset(); slow->Release(); Destroyed(slow); Reaped();
}
void HooksPoisonDuringVendorCall() {
  auto slow = std::make_shared<Control>(); slow->block = Call::kOpen;
  auto first = CreateLinuxBoundedBackendWithRetirementHooks(
      std::make_unique<FakeBackend>(slow), {"independent", true},
      {CheckHook, RecordHook}, Short());
  auto active = std::make_shared<Control>(); active->block = Call::kEncode;
  auto second = Backend(active, HardwareEncoderVendor::kNvenc, {2s, 2s, 2s, 2s});
  Require(second->Open(Configuration()) == Status::kOk);
  EncodeCaller caller{second.get(), Status::kOk, {}}; pthread_t thread;
  Require(pthread_create(&thread, nullptr, EncodeOnThread, &caller) == 0); active->WaitEntered();
  Require(first->Open(Configuration()) == Status::kRetirementFailed); slow->WaitEntered();
  Require(hook_records > 0 && CheckLinuxBackendRetirement() == Status::kRetirementFailed);
  active->Release(); Require(pthread_join(thread, nullptr) == 0);
  Require(caller.result == Status::kRetirementFailed && caller.packet.bytes.empty());
  first.reset(); second.reset(); slow->Release(); Destroyed(slow); Reaped();
}
void InvalidHooksNoDriver() {
  for (const auto hooks : {LinuxBackendRetirementHooks{nullptr, RecordHook},
                           LinuxBackendRetirementHooks{CheckHook, nullptr}}) {
    auto control = std::make_shared<Control>();
    auto backend = CreateLinuxBoundedBackendWithRetirementHooks(
        std::make_unique<FakeBackend>(control), {"independent", true}, hooks, Short());
    Require(backend->Open(Configuration()) == Status::kInvalidArgument);
    Require(control->opened == 0 && control->closed == 0 && control->destroyed == 1);
    Require(hook_records == 0); backend.reset(); Reaped();
  }
}
void* MetricTransfersOnThread(void*) {
  for (int i = 0; i < 100; ++i) {
    LinuxEncoderTransferTimer timer(LinuxEncoderMetricTransfer::kCudaUpload, 24);
    if (i % 2 == 0) { timer.Finish(true); timer.Finish(true); }
    // Odd iterations exercise destructor completion after failed work.
  }
  return nullptr;
}
void ConcurrentMetrics() {
  pthread_t first, second;
  Require(pthread_create(&first, nullptr, MetricTransfersOnThread, nullptr) == 0);
  Require(pthread_create(&second, nullptr, MetricTransfersOnThread, nullptr) == 0);
  Require(pthread_join(first, nullptr) == 0 && pthread_join(second, nullptr) == 0);
  const auto metrics = SnapshotLinuxHardwareEncoderMetrics();
  const auto& transfer = metrics.transfers[MetricIndex(LinuxEncoderMetricTransfer::kCudaUpload)];
  Require(transfer.measurement_available && transfer.started_calls == 200 && transfer.requested_bytes == 4800);
  Require(transfer.successful_calls == 100 && transfer.successful_bytes == 2400);
  HistogramValid(transfer.completed, 200);
  Require(!metrics.gpu_readback_bytes.has_value());
}
void InvalidArguments() {
  auto control = std::make_shared<Control>(); auto deadlines = Short(); deadlines.encode = 0ms;
  auto backend = Backend(control, HardwareEncoderVendor::kNvenc, deadlines);
  Require(backend->Open(Configuration()) == Status::kInvalidArgument);
  Require(control->opened == 0 && control->closed == 0); backend.reset(); Reaped();
}
}  // namespace
}  // namespace webrtc::external_encoder

int main() {
  using namespace webrtc::external_encoder;
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {
    {"same_owner_cached_info_owned_packet_close_reopen", Happy},
    {"concurrent_metrics_no_lost_bytes_failure_unknown_readback", ConcurrentMetrics},
    {"invalid_nv12_dimensions_strides_null", InvalidFrames},
    {"ordinary_open_failure_cleanup_retry", OrdinaryOpenFailure},
    {"ordinary_encode_failure_cleanup", OrdinaryEncodeFailure},
    {"open_timeout_sticky_poison_stable_error", OpenTimeout},
    {"encode_timeout_owned_input_late_output_bounded_destructor", EncodeTimeoutOwnedInput},
    {"set_rates_timeout", RatesTimeout}, {"close_timeout", CloseTimeout},
    {"bounded_wrapper_destructor", DestructorTimeout},
    {"bounded_inner_destructor", InnerDestructorTimeout},
    {"close_failure_sticky_retained_storage", CloseFailure},
    {"late_cleanup_failure_retained_storage", LateCleanupFailure},
    {"checked_thread_creation_failure_no_driver", ThreadFailure},
    {"registry_caps_normal_workers_reaps", CapacityActive},
    {"registry_caps_normal_and_retiring_workers", CapacityIncludesRetired},
    {"vendor_poison_before_call_new_instance", PoisonBeforeCall},
    {"vendor_poison_during_call_rejects_output", PoisonDuringCall},
    {"invalid_deadlines_no_worker", InvalidArguments},
    {"independent_hooks_timeout_sticky_check_late_output", IndependentHooksTimeout},
    {"invalid_retirement_hooks_no_driver", InvalidHooksNoDriver},
    {"cross_vendor_poison_rejects_inflight_output", CrossVendorPoisonDuringCall},
    {"independent_hooks_poison_rejects_vendor_inflight_output", HooksPoisonDuringVendorCall},
  };
  int passed = 0;
  for (const auto& test : tests) {
    const pid_t child = fork();
    if (child == 0) { test.run(); std::_Exit(0); }
    Require(child > 0); int result = 0;
    Require(waitpid(child, &result, 0) == child);
    if (!WIFEXITED(result) || WEXITSTATUS(result) != 0) {
      std::cerr << test.name << " FAIL\n"; return 1;
    }
    std::cout << test.name << " PASS\n"; ++passed;
  }
  std::cout << "linux_bounded_backend_fake_tests PASS checks=" << passed
            << " hardware=NOT_RUN\n";
  return 0;
}
