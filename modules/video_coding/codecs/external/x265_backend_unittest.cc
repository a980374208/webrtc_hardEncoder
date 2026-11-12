/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/array_view.h"
#include "common_video/h265/h265_common.h"
#include "common_video/h265/h265_sps_parser.h"
#include "modules/video_coding/codecs/external/x265_video_encoder_backend.h"
#include "test/gtest.h"

namespace webrtc::external_encoder {
namespace {

Config TestConfig() {
  Config config;
  config.codec = Codec::kH265;
  config.width = 160;
  // Exercises x265's conformance crop rather than treating coded == display.
  config.height = 130;
  config.bitrate_bps = 200000;
  config.framerate_fps = 30;
  return config;
}

class Nv12Frame {
 public:
  explicit Nv12Frame(const Config& config)
      : width_(config.width),
        height_(config.height),
        stride_(config.width + 13),
        y_(stride_ * height_, 0xee),
        uv_(stride_ * height_ / 2, 0xdd) {
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        y_[y * stride_ + x] = 16 + (x + y) % 200;
      }
    }
    for (int y = 0; y < height_ / 2; ++y) {
      for (int x = 0; x < width_; x += 2) {
        uv_[y * stride_ + x] = 80;
        uv_[y * stride_ + x + 1] = 176;
      }
    }
  }

  Frame Get(uint64_t id) const {
    return {y_.data(), uv_.data(), stride_, stride_, width_, height_, id};
  }

 private:
  int width_;
  int height_;
  int stride_;
  std::vector<uint8_t> y_;
  std::vector<uint8_t> uv_;
};

bool HasNal(const Packet& packet, H265::NaluType type) {
  for (const auto& nal : H265::FindNaluIndices(packet.bytes)) {
    if (nal.payload_size >= 2 &&
        H265::ParseNaluType(packet.bytes[nal.payload_start_offset]) == type) {
      return true;
    }
  }
  return false;
}

std::optional<H265SpsParser::SpsState> Sps(const Packet& packet) {
  for (const auto& nal : H265::FindNaluIndices(packet.bytes)) {
    if (nal.payload_size > 2 &&
        H265::ParseNaluType(packet.bytes[nal.payload_start_offset]) ==
            H265::kSps) {
      return H265SpsParser::ParseSps(
          ArrayView<const uint8_t>(packet.bytes)
              .subview(nal.payload_start_offset + 2, nal.payload_size - 2));
    }
  }
  return std::nullopt;
}

void ExpectIdr(const Packet& packet, uint64_t id) {
  EXPECT_EQ(packet.frame_id, id);
  EXPECT_TRUE(packet.key_frame);
  EXPECT_TRUE(HasNal(packet, H265::kIdrWRadl) || HasNal(packet, H265::kIdrNLp));
  EXPECT_TRUE(HasNal(packet, H265::kVps));
  EXPECT_TRUE(HasNal(packet, H265::kSps));
  EXPECT_TRUE(HasNal(packet, H265::kPps));
  EXPECT_GE(packet.qp, 0);
  EXPECT_LE(packet.qp, 51);
}

TEST(X265BackendTest, ReportsSoftwareAndProbesRealMainEncoder) {
  const auto backend = CreateX265H265Backend();
  EXPECT_EQ(backend->GetInfo().implementation_name, "x265 H265");
  EXPECT_FALSE(backend->GetInfo().is_hardware_accelerated);
  std::string reason = "old error";
  EXPECT_EQ(ProbeX265H265Backend(&reason, 30), Status::kOk) << reason;
  EXPECT_TRUE(reason.empty());
  EXPECT_EQ(ProbeX265H265Backend(&reason, 31), Status::kInvalidArgument);
  EXPECT_FALSE(reason.empty());
}

TEST(X265BackendTest,
     ExperimentalProfilesAreExplicitAndUnknownNamesFailClosed) {
  for (const std::string_view name : {"", "baseline"}) {
    const auto tuning = ParseX265ExperimentalTuning(name);
    ASSERT_TRUE(tuning);
    EXPECT_EQ(tuning->wpp_pool_threads, 0);
    EXPECT_TRUE(tuning->strict_cbr);
    EXPECT_EQ(tuning->min_cu_size, 8);
    EXPECT_FALSE(tuning->split_rd_skip);
  }
  for (const auto name : {"wpp4", "wpp8", "wpp16", "wpp8-nostrict",
                          "baseline-cu16", "wpp8-cu16", "wpp8-rdskip"}) {
    SCOPED_TRACE(name);
    const auto tuning = ParseX265ExperimentalTuning(name);
    ASSERT_TRUE(tuning);
    EXPECT_EQ(tuning->wpp_pool_threads,
              std::string_view(name) == "baseline-cu16"
                  ? 0
                  : (std::string_view(name) == "wpp4"
                         ? 4
                         : (std::string_view(name) == "wpp16" ? 16 : 8)));
    EXPECT_EQ(tuning->strict_cbr, std::string_view(name) != "wpp8-nostrict");
    EXPECT_EQ(
        tuning->min_cu_size,
        std::string_view(name).find("cu16") != std::string_view::npos ? 16 : 8);
    EXPECT_EQ(tuning->split_rd_skip, std::string_view(name) == "wpp8-rdskip");
  }
  for (const auto name : {"wpp", "wpp0", "wpp32", "Wpp8", "wpp8 ",
                          "Enabled-wpp8", "baseline-nostrict", "cu32"}) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(ParseX265ExperimentalTuning(name));
  }
}

TEST(X265BackendTest, RejectsUnsupportedExperimentalOptionsBeforeOpening) {
  for (const int pool_threads : {-1, 1, 3, 7, 32}) {
    SCOPED_TRACE(pool_threads);
    auto backend = CreateX265H265Backend({pool_threads, true, 8});
    EXPECT_EQ(backend->Open(TestConfig()), Status::kInvalidArgument);
    EXPECT_NE(backend->LastError().find("tuning"), std::string_view::npos);
  }
  for (const int min_cu_size : {-1, 0, 4, 12, 32}) {
    SCOPED_TRACE(min_cu_size);
    auto backend = CreateX265H265Backend({8, true, min_cu_size});
    EXPECT_EQ(backend->Open(TestConfig()), Status::kInvalidArgument);
  }
}

TEST(X265BackendTest, RejectsIneffectiveWppInsteadOfSilentlyFallingBack) {
  auto backend = CreateX265H265Backend({8, true, 8});
  auto config = TestConfig();
  config.width = 64;  // x265 disables WPP with fewer than three CTU columns.
  EXPECT_EQ(backend->Open(config), Status::kUnsupported);
  EXPECT_FALSE(backend->LastError().empty());
  Packet packet;
  EXPECT_EQ(backend->Encode(Frame{}, true, &packet), Status::kError);
  EXPECT_TRUE(packet.bytes.empty());
}

class X265ExperimentalBackendTest
    : public ::testing::TestWithParam<const char*> {};

TEST_P(X265ExperimentalBackendTest,
       RetiresOnePictureWithIdrAndPreservesTuningAcrossRateReopen) {
  const auto tuning = ParseX265ExperimentalTuning(GetParam());
  ASSERT_TRUE(tuning);
  auto backend = CreateX265H265Backend(*tuning);
  const auto config = TestConfig();
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  const Nv12Frame source(config);
  Packet first;
  ASSERT_EQ(backend->Encode(source.Get(400), false, &first), Status::kOk)
      << backend->LastError();
  ExpectIdr(first, 400);
  const auto saved = first.bytes;
  const auto sps = Sps(first);
  ASSERT_TRUE(sps);
  EXPECT_EQ(sps->width, 160u);
  EXPECT_EQ(sps->height, 130u);
  EXPECT_EQ(sps->pic_height_in_luma_samples,
            tuning->min_cu_size == 16 ? 144u : 136u);
  Packet packet;
  ASSERT_EQ(backend->Encode(source.Get(403), false, &packet), Status::kOk)
      << backend->LastError();
  EXPECT_EQ(packet.frame_id, 403u);
  EXPECT_FALSE(packet.key_frame);
  EXPECT_EQ(first.bytes, saved);
  ASSERT_EQ(backend->Encode(source.Get(407), true, &packet), Status::kOk)
      << backend->LastError();
  ExpectIdr(packet, 407);
  // Open readback validates every tuning option again, including WPP, while
  // the public owner/PTS/forced-IDR contract remains unchanged.
  ASSERT_EQ(backend->SetRates({120000, 15}), Status::kOk)
      << backend->LastError();
  ASSERT_EQ(backend->Encode(source.Get(415), false, &packet), Status::kOk)
      << backend->LastError();
  ExpectIdr(packet, 415);
  ASSERT_EQ(backend->Encode(source.Get(421), false, &packet), Status::kOk)
      << backend->LastError();
  EXPECT_EQ(packet.frame_id, 421u);
  EXPECT_FALSE(packet.key_frame);
  backend->Close();
  EXPECT_EQ(first.bytes, saved);
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  ASSERT_EQ(backend->Encode(source.Get(430), false, &packet), Status::kOk)
      << backend->LastError();
  ExpectIdr(packet, 430);
}

INSTANTIATE_TEST_SUITE_P(Profiles,
                         X265ExperimentalBackendTest,
                         ::testing::Values("baseline",
                                           "wpp4",
                                           "wpp8",
                                           "wpp16",
                                           "wpp8-nostrict",
                                           "baseline-cu16",
                                           "wpp8-cu16",
                                           "wpp8-rdskip"));

TEST(X265BackendTest, RejectsInvalidConfigurationBeforeOpening) {
  auto backend = CreateX265H265Backend();
  auto config = TestConfig();
  config.codec = Codec::kH264;
  EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  for (const int dimension : {-2, 0, 14, 159, 8194}) {
    SCOPED_TRACE(dimension);
    config = TestConfig();
    config.width = dimension;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
    config = TestConfig();
    config.height = dimension;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint8_t level : {0, 1, 31, 92, 94, 255}) {
    SCOPED_TRACE(static_cast<int>(level));
    config = TestConfig();
    config.h265_level = level;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint32_t bitrate : {0u, 65535001u}) {
    config = TestConfig();
    config.bitrate_bps = bitrate;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint32_t fps : {0u, 241u}) {
    config = TestConfig();
    config.framerate_fps = fps;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  EXPECT_NE(backend->LastError().find("invalid"), std::string_view::npos);
}

TEST(X265BackendTest,
     ReturnsImmediateOwnedPicturesAndRequestedIdrWithExactPts) {
  auto backend = CreateX265H265Backend();
  const auto config = TestConfig();
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  const Nv12Frame source(config);
  Packet packet;
  ASSERT_EQ(backend->Encode(source.Get(90000), false, &packet), Status::kOk)
      << backend->LastError();
  ExpectIdr(packet, 90000);
  const auto saved = packet.bytes;
  const auto sps = Sps(packet);
  ASSERT_TRUE(sps);
  EXPECT_EQ(sps->width, 160u);
  EXPECT_EQ(sps->height, 130u);
  EXPECT_EQ(sps->pic_height_in_luma_samples, 136u);
  EXPECT_EQ(sps->sps_max_sub_layers_minus1, 0u);
  EXPECT_EQ(sps->chroma_format_idc, 1u);
  EXPECT_EQ(sps->bit_depth_luma_minus8, 0u);
  Packet delta;
  ASSERT_EQ(backend->Encode(source.Get(93000), false, &delta), Status::kOk)
      << backend->LastError();
  EXPECT_EQ(delta.frame_id, 93000u);
  EXPECT_FALSE(delta.key_frame);
  EXPECT_TRUE(HasNal(delta, H265::kTrailR) || HasNal(delta, H265::kTrailN));
  EXPECT_EQ(packet.bytes, saved);
  ASSERT_EQ(backend->Encode(source.Get(96000), true, &delta), Status::kOk)
      << backend->LastError();
  ExpectIdr(delta, 96000);
  backend->Close();
  EXPECT_EQ(packet.bytes, saved);
}

TEST(X265BackendTest, RatesReopenForcesIdrAndPreservesCallerIds) {
  auto backend = CreateX265H265Backend();
  const auto config = TestConfig();
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  const Nv12Frame source(config);
  Packet packet;
  ASSERT_EQ(backend->Encode(source.Get(17), false, &packet), Status::kOk);
  ASSERT_EQ(backend->SetRates({120000, 15}), Status::kOk)
      << backend->LastError();
  ASSERT_EQ(backend->Encode(source.Get(18), false, &packet), Status::kOk)
      << backend->LastError();
  ExpectIdr(packet, 18);
  ASSERT_EQ(backend->SetRates({120000, 15}), Status::kOk);
  ASSERT_EQ(backend->Encode(source.Get(19), false, &packet), Status::kOk);
  EXPECT_FALSE(packet.key_frame);
  EXPECT_EQ(packet.frame_id, 19u);
  ASSERT_EQ(backend->SetRates({180000, 15}), Status::kOk)
      << backend->LastError();
  ASSERT_EQ(backend->Encode(source.Get(20), false, &packet), Status::kOk);
  ExpectIdr(packet, 20);
}

TEST(X265BackendTest, InvalidInputAndRatesKeepTheValidSessionUsable) {
  auto backend = CreateX265H265Backend();
  const auto config = TestConfig();
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  const Nv12Frame source(config);
  Packet packet;
  auto frame = source.Get(1);
  frame.stride_uv = config.width - 1;
  EXPECT_EQ(backend->Encode(frame, false, &packet), Status::kInvalidArgument);
  frame = source.Get(1);
  frame.y = nullptr;
  EXPECT_EQ(backend->Encode(frame, false, &packet), Status::kInvalidArgument);
  frame = source.Get(1);
  frame.height += 2;
  EXPECT_EQ(backend->Encode(frame, false, &packet), Status::kInvalidArgument);
  EXPECT_EQ(backend->Encode(source.Get(std::numeric_limits<uint64_t>::max()),
                            false, &packet),
            Status::kInvalidArgument);
  EXPECT_EQ(backend->Encode(source.Get(1), false, nullptr),
            Status::kInvalidArgument);
  EXPECT_EQ(backend->SetRates({0, 30}), Status::kInvalidArgument);
  EXPECT_EQ(backend->SetRates({200000, 0}), Status::kInvalidArgument);
  ASSERT_EQ(backend->Encode(source.Get(1), false, &packet), Status::kOk)
      << backend->LastError();
  ExpectIdr(packet, 1);
}

TEST(X265BackendTest, RejectsARequestThatExceedsNegotiatedLevel) {
  auto backend = CreateX265H265Backend();
  auto config = TestConfig();
  config.width = 640;
  config.height = 360;
  config.h265_level = 30;
  EXPECT_EQ(backend->Open(config), Status::kUnsupported);
  EXPECT_FALSE(backend->LastError().empty());
  Packet packet;
  EXPECT_EQ(backend->Encode(Frame{}, true, &packet), Status::kError);
  EXPECT_TRUE(packet.bytes.empty());
}

TEST(X265BackendTest, DoesNotSilentlyAcceptLevelClampedRateOnReopen) {
  auto backend = CreateX265H265Backend();
  auto config = TestConfig();
  config.height = 128;
  config.h265_level = 30;
  config.framerate_fps = 15;
  config.bitrate_bps = 64000;
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  EXPECT_EQ(backend->SetRates({65535000, 15}), Status::kUnsupported);
  EXPECT_FALSE(backend->LastError().empty());
  const Nv12Frame source(config);
  Packet packet;
  EXPECT_EQ(backend->Encode(source.Get(1), false, &packet), Status::kError);
  EXPECT_TRUE(packet.bytes.empty());
}

TEST(X265BackendTest, CloseIsIdempotentAndOpenStartsARealNewSequence) {
  auto backend = CreateX265H265Backend();
  const auto config = TestConfig();
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  const Nv12Frame source(config);
  Packet packet;
  ASSERT_EQ(backend->Encode(source.Get(100), false, &packet), Status::kOk);
  backend->Close();
  backend->Close();
  EXPECT_EQ(backend->Encode(source.Get(101), true, &packet), Status::kError);
  EXPECT_TRUE(packet.bytes.empty());
  EXPECT_EQ(backend->SetRates({200000, 30}), Status::kError);
  ASSERT_EQ(backend->Open(config), Status::kOk) << backend->LastError();
  ASSERT_EQ(backend->Encode(source.Get(101), false, &packet), Status::kOk)
      << backend->LastError();
  ExpectIdr(packet, 101);
}

}  // namespace
}  // namespace webrtc::external_encoder
