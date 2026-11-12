/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "test/gtest.h"

namespace webrtc::external_encoder {
namespace {

constexpr std::array<HardwareEncoderVendor, 3> kVendors = {
    HardwareEncoderVendor::kNvenc, HardwareEncoderVendor::kQsv,
    HardwareEncoderVendor::kAmf};
constexpr auto kStartTimeout = std::chrono::seconds(5);
constexpr auto kBlockedObservation = std::chrono::milliseconds(100);

// Test-owned signals keep a mock probe inside its cleanup phase. No SDK or
// process-global coordinator is touched. Every test releases and joins its
// threads even when an expectation fails.
class Signal {
 public:
  void Set() {
    std::lock_guard<std::mutex> lock(mutex_);
    set_ = true;
    changed_.notify_all();
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return set_; });
  }

  bool WaitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return set_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool set_ = false;
};

TEST(HardwareEncoderProbeTest, FreshCoordinatorClearsStaleDiagnostics) {
  HardwareEncoderProbeCoordinator coordinator;
  for (const auto vendor : kVendors) {
    std::string reason = "stale failure";
    EXPECT_EQ(coordinator.Check(vendor, &reason), Status::kOk);
    EXPECT_TRUE(reason.empty());
  }
  std::string reason = "stale failure";
  EXPECT_EQ(coordinator.CheckAny(&reason), Status::kOk);
  EXPECT_TRUE(reason.empty());
}

TEST(HardwareEncoderProbeTest, OrdinaryFailuresRemainRetryableForEachVendor) {
  const std::array<Status, 6> ordinary_failures = {
      Status::kUnavailable, Status::kUnsupported, Status::kInvalidArgument,
      Status::kNoOutput,    Status::kTimeout,     Status::kError};
  for (const auto vendor : kVendors) {
    for (const auto failure : ordinary_failures) {
      SCOPED_TRACE(static_cast<int>(vendor));
      SCOPED_TRACE(static_cast<int>(failure));
      HardwareEncoderProbeCoordinator coordinator;
      int calls = 0;
      std::string reason;
      EXPECT_EQ(coordinator.Run(vendor, &reason,
                                [&](std::string* diagnostic) {
                                  ++calls;
                                  *diagnostic = "ordinary probe failure";
                                  return failure;
                                }),
                failure);
      EXPECT_EQ(reason, "ordinary probe failure");
      EXPECT_EQ(coordinator.Check(vendor), Status::kOk);
      EXPECT_EQ(coordinator.CheckAny(), Status::kOk);
      EXPECT_EQ(coordinator.Run(vendor, &reason,
                                [&](std::string* diagnostic) {
                                  ++calls;
                                  diagnostic->clear();
                                  return Status::kOk;
                                }),
                Status::kOk);
      EXPECT_EQ(calls, 2);
      EXPECT_TRUE(reason.empty());
    }
  }
}

TEST(HardwareEncoderProbeTest,
     RetirementFailureBlocksNewCodecCallbackAndPreservesFirstReason) {
  for (const auto vendor : kVendors) {
    SCOPED_TRACE(static_cast<int>(vendor));
    HardwareEncoderProbeCoordinator coordinator;
    int h264_calls = 0;
    int h265_calls = 0;
    std::string reason;
    EXPECT_EQ(coordinator.Run(vendor, &reason,
                              [&](std::string* diagnostic) {
                                ++h264_calls;
                                *diagnostic = "H264 SDK retirement failed";
                                return Status::kRetirementFailed;
                              }),
              Status::kRetirementFailed);
    EXPECT_EQ(reason, "H264 SDK retirement failed");
    // The API keys the gate by vendor, not codec or callback/backend identity.
    EXPECT_EQ(coordinator.Run(vendor, &reason,
                              [&](std::string* diagnostic) {
                                ++h265_calls;
                                *diagnostic = "later H265 diagnostic";
                                return Status::kOk;
                              }),
              Status::kRetirementFailed);
    EXPECT_EQ(h264_calls, 1);
    EXPECT_EQ(h265_calls, 0);
    EXPECT_EQ(reason, "H264 SDK retirement failed");
    reason = "stale replacement";
    EXPECT_EQ(coordinator.Check(vendor, &reason), Status::kRetirementFailed);
    EXPECT_EQ(reason, "H264 SDK retirement failed");
  }
}

TEST(HardwareEncoderProbeTest,
     MissingDiagnosticAndNullOutputStillRetainRetirementFailure) {
  HardwareEncoderProbeCoordinator coordinator;
  int calls = 0;
  EXPECT_EQ(coordinator.Run(HardwareEncoderVendor::kAmf, nullptr,
                            [&](std::string* diagnostic) {
                              ++calls;
                              EXPECT_NE(diagnostic, nullptr);
                              return Status::kRetirementFailed;
                            }),
            Status::kRetirementFailed);
  std::string first_reason;
  EXPECT_EQ(coordinator.Check(HardwareEncoderVendor::kAmf, &first_reason),
            Status::kRetirementFailed);
  EXPECT_FALSE(first_reason.empty());
  EXPECT_EQ(coordinator.Run(HardwareEncoderVendor::kAmf, nullptr,
                            [&](std::string*) {
                              ++calls;
                              return Status::kOk;
                            }),
            Status::kRetirementFailed);
  std::string reason;
  EXPECT_EQ(coordinator.Check(HardwareEncoderVendor::kAmf, &reason),
            Status::kRetirementFailed);
  EXPECT_EQ(reason, first_reason);
  EXPECT_EQ(calls, 1);
}

TEST(HardwareEncoderProbeTest,
     CheckAnyRejectsPoisonedVendorWhileExplicitVendorsRemainIndependent) {
  HardwareEncoderProbeCoordinator coordinator;
  EXPECT_EQ(coordinator.Run(HardwareEncoderVendor::kAmf, nullptr,
                            [](std::string* diagnostic) {
                              *diagnostic = "context cleanup failed";
                              return Status::kRetirementFailed;
                            }),
            Status::kRetirementFailed);
  int independent_calls = 0;
  for (const auto vendor :
       {HardwareEncoderVendor::kNvenc, HardwareEncoderVendor::kQsv}) {
    EXPECT_EQ(coordinator.Check(vendor), Status::kOk);
    EXPECT_EQ(coordinator.Run(vendor, nullptr,
                              [&](std::string*) {
                                ++independent_calls;
                                return Status::kOk;
                              }),
              Status::kOk);
  }
  EXPECT_EQ(independent_calls, 2);
  std::string reason;
  EXPECT_EQ(coordinator.CheckAny(&reason), Status::kRetirementFailed);
  EXPECT_EQ(reason, "AMF: context cleanup failed");
  EXPECT_EQ(coordinator.Check(HardwareEncoderVendor::kAmf),
            Status::kRetirementFailed);
}

TEST(HardwareEncoderProbeTest, SerializesProbeAndCleanupAcrossVendors) {
  HardwareEncoderProbeCoordinator coordinator;
  Signal first_entered, release_cleanup, second_started, second_entered;
  std::atomic<bool> cleanup_completed = false;
  bool second_observed_cleanup_completed = false;
  Status first_result = Status::kError;
  Status second_result = Status::kError;
  std::thread first([&] {
    first_result = coordinator.Run(HardwareEncoderVendor::kAmf, nullptr,
                                   [&](std::string*) {
                                     first_entered.Set();
                                     release_cleanup.Wait();
                                     cleanup_completed.store(true);
                                     return Status::kOk;
                                   });
  });
  EXPECT_TRUE(first_entered.WaitFor(kStartTimeout));
  std::thread second([&] {
    second_started.Set();
    second_result = coordinator.Run(
        HardwareEncoderVendor::kQsv, nullptr, [&](std::string*) {
          second_observed_cleanup_completed = cleanup_completed.load();
          second_entered.Set();
          return Status::kOk;
        });
  });
  EXPECT_TRUE(second_started.WaitFor(kStartTimeout));
  EXPECT_FALSE(second_entered.WaitFor(kBlockedObservation));
  release_cleanup.Set();
  first.join();
  second.join();
  EXPECT_EQ(first_result, Status::kOk);
  EXPECT_EQ(second_result, Status::kOk);
  EXPECT_TRUE(second_observed_cleanup_completed);
}

TEST(HardwareEncoderProbeTest, CallbackCanCheckAndPublishRetirementFailure) {
  HardwareEncoderProbeCoordinator coordinator;
  std::string reason;
  EXPECT_EQ(coordinator.Run(
                HardwareEncoderVendor::kAmf, &reason,
                [&](std::string* diagnostic) {
                  EXPECT_EQ(coordinator.Check(HardwareEncoderVendor::kAmf),
                            Status::kOk);
                  EXPECT_EQ(coordinator.CheckAny(), Status::kOk);
                  coordinator.RecordRetirementFailure(
                      HardwareEncoderVendor::kAmf, "actual Close failed");
                  coordinator.RecordRetirementFailure(
                      HardwareEncoderVendor::kAmf, "later wrapper diagnostic");
                  std::string stored_reason;
                  EXPECT_EQ(coordinator.Check(HardwareEncoderVendor::kAmf,
                                              &stored_reason),
                            Status::kRetirementFailed);
                  EXPECT_EQ(stored_reason, "actual Close failed");
                  EXPECT_EQ(coordinator.CheckAny(), Status::kRetirementFailed);
                  *diagnostic = "outer probe diagnostic";
                  return Status::kRetirementFailed;
                }),
            Status::kRetirementFailed);
  EXPECT_EQ(reason, "actual Close failed");
}

TEST(HardwareEncoderProbeTest,
     PublishedRuntimeFailureBlocksNewProbeAndPreservesFirstReason) {
  HardwareEncoderProbeCoordinator coordinator;
  coordinator.RecordRetirementFailure(HardwareEncoderVendor::kAmf,
                                      "runtime Close failed");
  coordinator.RecordRetirementFailure(HardwareEncoderVendor::kAmf,
                                      "later failure");
  int calls = 0;
  std::string reason;
  EXPECT_EQ(coordinator.Run(HardwareEncoderVendor::kAmf, &reason,
                            [&](std::string*) {
                              ++calls;
                              return Status::kOk;
                            }),
            Status::kRetirementFailed);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(reason, "runtime Close failed");
  EXPECT_EQ(coordinator.Check(HardwareEncoderVendor::kQsv), Status::kOk);
  EXPECT_EQ(coordinator.CheckAny(), Status::kRetirementFailed);
}

TEST(HardwareEncoderProbeTest,
     ConcurrentRuntimeFailureOverridesOtherwiseSuccessfulProbe) {
  HardwareEncoderProbeCoordinator coordinator;
  Signal probe_entered, release_cleanup, runtime_published;
  Status result = Status::kError;
  std::string reason;
  std::thread probe([&] {
    result = coordinator.Run(HardwareEncoderVendor::kAmf, &reason,
                             [&](std::string*) {
                               probe_entered.Set();
                               release_cleanup.Wait();
                               return Status::kOk;
                             });
  });
  EXPECT_TRUE(probe_entered.WaitFor(kStartTimeout));
  std::thread runtime_close([&] {
    coordinator.RecordRetirementFailure(HardwareEncoderVendor::kAmf,
                                        "concurrent runtime Close failed");
    runtime_published.Set();
  });
  EXPECT_TRUE(runtime_published.WaitFor(kStartTimeout));
  release_cleanup.Set();
  probe.join();
  runtime_close.join();
  EXPECT_EQ(result, Status::kRetirementFailed);
  EXPECT_EQ(reason, "concurrent runtime Close failed");
}

TEST(HardwareEncoderProbeTest,
     WaitingSameVendorCannotRetryAfterInFlightRetirementFailure) {
  HardwareEncoderProbeCoordinator coordinator;
  Signal first_entered, release_cleanup, second_started, second_entered;
  int second_calls = 0;
  Status first_result = Status::kError;
  Status second_result = Status::kError;
  std::string second_reason;
  std::thread first([&] {
    first_result = coordinator.Run(HardwareEncoderVendor::kAmf, nullptr,
                                   [&](std::string* diagnostic) {
                                     first_entered.Set();
                                     release_cleanup.Wait();
                                     *diagnostic = "first cleanup failure";
                                     return Status::kRetirementFailed;
                                   });
  });
  EXPECT_TRUE(first_entered.WaitFor(kStartTimeout));
  std::thread second([&] {
    second_started.Set();
    second_result = coordinator.Run(HardwareEncoderVendor::kAmf, &second_reason,
                                    [&](std::string*) {
                                      ++second_calls;
                                      second_entered.Set();
                                      return Status::kOk;
                                    });
  });
  EXPECT_TRUE(second_started.WaitFor(kStartTimeout));
  EXPECT_FALSE(second_entered.WaitFor(kBlockedObservation));
  release_cleanup.Set();
  first.join();
  second.join();
  EXPECT_EQ(first_result, Status::kRetirementFailed);
  EXPECT_EQ(second_result, Status::kRetirementFailed);
  EXPECT_EQ(second_calls, 0);
  EXPECT_EQ(second_reason, "first cleanup failure");
}

}  // namespace
}  // namespace webrtc::external_encoder
