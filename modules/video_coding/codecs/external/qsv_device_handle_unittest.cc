/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/qsv_device_handle.h"

#include "test/gtest.h"

namespace webrtc::external_encoder {
namespace {

struct FakeDevice {
  unsigned long AddRef() { return ++references; }
  unsigned long Release() { return --references; }
  unsigned long references = 1;
};

TEST(QsvDeviceHandleTest, LegacyBorrowedHandlePreservesSessionDeviceOwner) {
  FakeDevice device;
  {
    auto validation_reference = RetainQsvDeviceHandle(&device, 1);
    EXPECT_EQ(validation_reference.Get(), &device);
    EXPECT_EQ(device.references, 2u);
  }
  // OpenAdapter locals must not retire the device before probe/session Close.
  EXPECT_EQ(device.references, 1u);
  EXPECT_EQ(device.Release(), 0u);
}

TEST(QsvDeviceHandleTest, OneVplOwnedHandleReleasesReturnedReference) {
  FakeDevice device;
  device.AddRef();  // API 2 GetHandle returns an added reference.
  {
    auto validation_reference = RetainQsvDeviceHandle(&device, 2);
    EXPECT_EQ(validation_reference.Get(), &device);
    EXPECT_EQ(device.references, 2u);
  }
  EXPECT_EQ(device.references, 1u);
  EXPECT_EQ(device.Release(), 0u);
}

}  // namespace
}  // namespace webrtc::external_encoder
