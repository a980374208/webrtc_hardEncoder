/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_CAPABILITY_QUERY_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_CAPABILITY_QUERY_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace webrtc::external_encoder::internal {

enum class CapabilityQueryError {
  kNone,
  kCountCallFailed,
  kEmptyCount,
  kExcessiveCount,
  kListCallFailed,
  kInvalidFilledCount,
};

struct CapabilityQueryResult {
  CapabilityQueryError error = CapabilityQueryError::kNone;
  uint32_t queried_count = 0;
  uint32_t filled_count = 0;
  uint32_t count_status = 0;
  std::optional<uint32_t> list_status;

  bool ok() const { return error == CapabilityQueryError::kNone; }
};

// NvEncodeAPI list capacities must match the preceding count query. Keep the
// filled count separate, reject inconsistent driver output, and expose only
// entries actually filled. Callbacks return the SDK status (zero is success).
// The bound prevents an implausible count from causing an excessive allocation.
template <typename Value, typename GetCount, typename GetList>
CapabilityQueryResult QueryNvencCapabilities(GetCount get_count,
                                             GetList get_list,
                                             std::vector<Value>* values) {
  values->clear();
  CapabilityQueryResult result;
  result.count_status = static_cast<uint32_t>(get_count(&result.queried_count));
  if (result.count_status != 0) {
    result.error = CapabilityQueryError::kCountCallFailed;
    return result;
  }
  if (result.queried_count == 0) {
    result.error = CapabilityQueryError::kEmptyCount;
    return result;
  }
  constexpr uint32_t kMaximumCapabilityCount = 256;
  if (result.queried_count > kMaximumCapabilityCount) {
    result.error = CapabilityQueryError::kExcessiveCount;
    return result;
  }
  values->resize(result.queried_count);
  result.list_status = static_cast<uint32_t>(
      get_list(values->data(), result.queried_count, &result.filled_count));
  if (*result.list_status != 0) {
    result.error = CapabilityQueryError::kListCallFailed;
    values->clear();
    return result;
  }
  if (result.filled_count == 0 || result.filled_count > result.queried_count) {
    result.error = CapabilityQueryError::kInvalidFilledCount;
    values->clear();
    return result;
  }
  values->resize(result.filled_count);
  return result;
}

}  // namespace webrtc::external_encoder::internal

#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_CAPABILITY_QUERY_H_
