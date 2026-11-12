/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#include "modules/video_coding/codecs/external/nvenc_capability_query.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "test/gtest.h"

namespace webrtc::external_encoder::internal {
namespace {

// Deliberately not a uint32_t: vendor APIs return enum status values.
enum class FakeSdkStatus : uint32_t { kSuccess = 0, kFailure = 17 };

TEST(NvencCapabilityQueryTest, UsesQueriedCapacityAndFindsNonFirstCapability) {
  bool count_called = false;
  bool list_called = false;
  std::vector<int> capabilities;
  const auto result = QueryNvencCapabilities<int>(
      [&](uint32_t* count) {
        count_called = true;
        *count = 2;
        return FakeSdkStatus::kSuccess;
      },
      [&](int* values, uint32_t capacity, uint32_t* filled) {
        EXPECT_TRUE(count_called);
        list_called = true;
        EXPECT_EQ(capacity, 2u);
        // Reproduce a contract-sensitive enumerator. A caller passing a
        // guessed large capacity gets success without populated output.
        if (capacity != 2) {
          *filled = 0;
          return FakeSdkStatus::kSuccess;
        }
        values[0] = 11;
        values[1] = 42;
        *filled = 2;
        return FakeSdkStatus::kSuccess;
      },
      &capabilities);
  EXPECT_TRUE(list_called);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.error, CapabilityQueryError::kNone);
  EXPECT_EQ(result.queried_count, 2u);
  EXPECT_EQ(result.filled_count, 2u);
  EXPECT_EQ(result.count_status, 0u);
  ASSERT_TRUE(result.list_status.has_value());
  EXPECT_EQ(*result.list_status, 0u);
  ASSERT_EQ(capabilities.size(), 2u);
  EXPECT_EQ(capabilities[0], 11);
  EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), 42),
            capabilities.end());
}

TEST(NvencCapabilityQueryTest, EmptyCountDoesNotCallListOrKeepOldOutput) {
  bool list_called = false;
  std::vector<int> capabilities = {99};
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t* count) {
        *count = 0;
        return 0u;
      },
      [&](int*, uint32_t, uint32_t*) {
        list_called = true;
        return 0u;
      },
      &capabilities);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, CapabilityQueryError::kEmptyCount);
  EXPECT_EQ(result.queried_count, 0u);
  EXPECT_EQ(result.count_status, 0u);
  EXPECT_FALSE(result.list_status.has_value());
  EXPECT_FALSE(list_called);
  EXPECT_TRUE(capabilities.empty());
}

TEST(NvencCapabilityQueryTest, CountFailurePreservesVendorStatus) {
  bool list_called = false;
  std::vector<int> capabilities = {99};
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t*) { return FakeSdkStatus::kFailure; },
      [&](int*, uint32_t, uint32_t*) {
        list_called = true;
        return FakeSdkStatus::kSuccess;
      },
      &capabilities);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, CapabilityQueryError::kCountCallFailed);
  EXPECT_EQ(result.count_status, 17u);
  EXPECT_FALSE(result.list_status.has_value());
  EXPECT_FALSE(list_called);
  EXPECT_TRUE(capabilities.empty());
}

TEST(NvencCapabilityQueryTest, ExcessiveCountDoesNotAllocateOrCallList) {
  bool list_called = false;
  std::vector<int> capabilities = {99};
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t* count) {
        *count = 257;
        return 0u;
      },
      [&](int*, uint32_t, uint32_t*) {
        list_called = true;
        return 0u;
      },
      &capabilities);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, CapabilityQueryError::kExcessiveCount);
  EXPECT_EQ(result.queried_count, 257u);
  EXPECT_FALSE(result.list_status.has_value());
  EXPECT_FALSE(list_called);
  EXPECT_TRUE(capabilities.empty());
}

TEST(NvencCapabilityQueryTest, ListFailureDoesNotExposePartiallyWrittenValues) {
  std::vector<int> capabilities = {99};
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t* count) {
        *count = 2;
        return 0u;
      },
      [](int* values, uint32_t capacity, uint32_t* filled) {
        EXPECT_EQ(capacity, 2u);
        values[0] = 42;
        *filled = 1;
        return 23u;
      },
      &capabilities);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, CapabilityQueryError::kListCallFailed);
  EXPECT_EQ(result.queried_count, 2u);
  EXPECT_EQ(result.count_status, 0u);
  ASSERT_TRUE(result.list_status.has_value());
  EXPECT_EQ(*result.list_status, 23u);
  EXPECT_TRUE(capabilities.empty());
}

TEST(NvencCapabilityQueryTest, SuccessWithZeroFilledIsAnEnumerationError) {
  std::vector<int> capabilities = {99};
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t* count) {
        *count = 2;
        return 0u;
      },
      [](int*, uint32_t capacity, uint32_t* filled) {
        EXPECT_EQ(capacity, 2u);
        *filled = 0;
        return 0u;
      },
      &capabilities);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, CapabilityQueryError::kInvalidFilledCount);
  EXPECT_EQ(result.queried_count, 2u);
  EXPECT_EQ(result.filled_count, 0u);
  ASSERT_TRUE(result.list_status.has_value());
  EXPECT_EQ(*result.list_status, 0u);
  EXPECT_TRUE(capabilities.empty());
}

TEST(NvencCapabilityQueryTest, FilledCountCannotExceedQueriedCapacity) {
  std::vector<int> capabilities = {99};
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t* count) {
        *count = 2;
        return 0u;
      },
      [](int* values, uint32_t capacity, uint32_t* filled) {
        EXPECT_EQ(capacity, 2u);
        values[0] = 42;
        *filled = 3;
        return 0u;
      },
      &capabilities);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, CapabilityQueryError::kInvalidFilledCount);
  EXPECT_EQ(result.queried_count, 2u);
  EXPECT_EQ(result.filled_count, 3u);
  EXPECT_TRUE(capabilities.empty());
}

TEST(NvencCapabilityQueryTest, ShrinksOutputToNonzeroActualCount) {
  std::vector<int> capabilities = {99};
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t* count) {
        *count = 3;
        return 0u;
      },
      [](int* values, uint32_t capacity, uint32_t* filled) {
        EXPECT_EQ(capacity, 3u);
        values[0] = 42;
        *filled = 1;
        return 0u;
      },
      &capabilities);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.queried_count, 3u);
  EXPECT_EQ(result.filled_count, 1u);
  ASSERT_EQ(capabilities.size(), 1u);
  EXPECT_EQ(capabilities[0], 42);
}

TEST(NvencCapabilityQueryTest, AcceptsCountAtSafetyLimit) {
  std::vector<int> capabilities;
  const auto result = QueryNvencCapabilities<int>(
      [](uint32_t* count) {
        *count = 256;
        return 0u;
      },
      [](int* values, uint32_t capacity, uint32_t* filled) {
        EXPECT_EQ(capacity, 256u);
        std::fill(values, values + capacity, 42);
        *filled = capacity;
        return 0u;
      },
      &capabilities);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(capabilities.size(), 256u);
}

}  // namespace
}  // namespace webrtc::external_encoder::internal
