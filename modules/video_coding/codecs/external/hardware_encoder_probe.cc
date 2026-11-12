/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"

#include <cstddef>
#include <utility>

#include "rtc_base/logging.h"

namespace webrtc::external_encoder {
namespace {
size_t Index(HardwareEncoderVendor vendor) {
  return static_cast<size_t>(vendor);
}
const char* Name(HardwareEncoderVendor vendor) {
  switch (vendor) {
    case HardwareEncoderVendor::kNvenc:
      return "NVENC";
    case HardwareEncoderVendor::kQsv:
      return "QSV";
    case HardwareEncoderVendor::kAmf:
      return "AMF";
  }
  return "unknown";
}
}  // namespace

Status HardwareEncoderProbeCoordinator::Run(
    HardwareEncoderVendor vendor,
    std::string* reason,
    const std::function<Status(std::string*)>& probe) {
  std::lock_guard<std::mutex> lock(probe_mutex_);
  std::string diagnostic;
  if (Check(vendor, &diagnostic) == Status::kRetirementFailed) {
    if (reason) {
      *reason = diagnostic;
    }
    RTC_LOG(LS_ERROR) << "Hardware probe requested=" << Name(vendor)
                      << " action=blocked reason=prior_retirement_failure "
                      << diagnostic;
    return Status::kRetirementFailed;
  }
  Status status = probe(&diagnostic);
  if (status == Status::kRetirementFailed) {
    RecordRetirementFailure(vendor, diagnostic);
  }
  // Actual Close publishes directly. A concurrent runtime cleanup failure is
  // fatal even when this particular probe callback otherwise returned success.
  std::string retirement_reason;
  if (Check(vendor, &retirement_reason) == Status::kRetirementFailed) {
    status = Status::kRetirementFailed;
    diagnostic = std::move(retirement_reason);
  }
  if (reason) {
    *reason = std::move(diagnostic);
  }
  return status;
}

Status HardwareEncoderProbeCoordinator::Check(HardwareEncoderVendor vendor,
                                              std::string* reason) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto& failure = failures_[Index(vendor)];
  if (reason) {
    *reason = failure.failed ? failure.reason : std::string();
  }
  return failure.failed ? Status::kRetirementFailed : Status::kOk;
}

Status HardwareEncoderProbeCoordinator::CheckAny(std::string* reason) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  for (size_t index = 0; index < failures_.size(); ++index) {
    if (failures_[index].failed) {
      if (reason) {
        *reason = std::string(Name(static_cast<HardwareEncoderVendor>(index))) +
                  ": " + failures_[index].reason;
      }
      return Status::kRetirementFailed;
    }
  }
  if (reason) {
    reason->clear();
  }
  return Status::kOk;
}

void HardwareEncoderProbeCoordinator::RecordRetirementFailure(
    HardwareEncoderVendor vendor,
    std::string_view reason) {
  std::string diagnostic;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto& failure = failures_[Index(vendor)];
    if (failure.failed) {
      return;
    }
    failure.failed = true;
    failure.reason =
        reason.empty() ? "SDK cleanup failed" : std::string(reason);
    diagnostic = failure.reason;
  }
  RTC_LOG(LS_ERROR) << "Hardware requested=" << Name(vendor)
                    << " action=blocked reason=retirement_failure "
                    << diagnostic;
}

HardwareEncoderProbeCoordinator& HardwareEncoderProbes() {
  // Process lifetime: late backend/global-object destructors may still publish
  // cleanup failures. Never destroy the synchronization state before them.
  static auto* const coordinator = new HardwareEncoderProbeCoordinator;
  return *coordinator;
}
}  // namespace webrtc::external_encoder
