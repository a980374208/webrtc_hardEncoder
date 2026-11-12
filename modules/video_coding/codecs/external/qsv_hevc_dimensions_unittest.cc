/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/qsv_hevc_dimensions.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "test/gtest.h"

namespace webrtc::external_encoder {
namespace {

std::vector<uint8_t> HexBytes(std::string_view text) {
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < text.size(); i += 2) {
    bytes.push_back((digit(text[i]) << 4) | digit(text[i + 1]));
  }
  return bytes;
}

// Real libx265 Main/tier0/level93, 8-bit, one-layer SPS. The exact fixture is
// shared with the adapter tests. The padded fixture was encoded at 32x18:
// FFmpeg trace_headers reports coded 32x24 and bottom conformance offset 3;
// its independent software decoder returns 32x18 I420 (864 bytes).
std::vector<uint8_t> ExactSps() {
  return HexBytes(
      "0000000142010101600000030090000003000003005da0421165baabc2e6808000000300"
      "8000000f04");
}

std::vector<uint8_t> PaddedSps() {
  return HexBytes(
      "0000000142010101600000030090000003000003005da04219f26592aaf2bc05a0200000"
      "03002000000303c1");
}

TEST(QsvHevcDimensionsTest, ParsesExactCodedAndDisplayDimensions) {
  const auto dimensions = ParseQsvHevcSpsDimensions(ExactSps());
  ASSERT_TRUE(dimensions);
  EXPECT_EQ(dimensions->coded_width, 32u);
  EXPECT_EQ(dimensions->coded_height, 16u);
  EXPECT_EQ(dimensions->display_width, 32u);
  EXPECT_EQ(dimensions->display_height, 16u);
  EXPECT_TRUE(QsvHevcDimensionsMatch(*dimensions, 32, 16, 32, 16, 32, 16));
}

TEST(QsvHevcDimensionsTest, MatchesPaddedCodedPictureAndConformanceCrop) {
  const auto dimensions = ParseQsvHevcSpsDimensions(PaddedSps());
  ASSERT_TRUE(dimensions);
  EXPECT_EQ(dimensions->coded_width, 32u);
  EXPECT_EQ(dimensions->coded_height, 24u);
  EXPECT_EQ(dimensions->display_width, 32u);
  EXPECT_EQ(dimensions->display_height, 18u);
  EXPECT_TRUE(QsvHevcDimensionsMatch(*dimensions, 32, 18, 32, 32, 32, 24));
}

TEST(QsvHevcDimensionsTest, RejectsDifferentRequestedDisplayDimensions) {
  const auto dimensions = ParseQsvHevcSpsDimensions(PaddedSps());
  ASSERT_TRUE(dimensions);
  EXPECT_FALSE(QsvHevcDimensionsMatch(*dimensions, 30, 18, 32, 32, 32, 24));
  EXPECT_FALSE(QsvHevcDimensionsMatch(*dimensions, 32, 24, 32, 32, 32, 24));
}

TEST(QsvHevcDimensionsTest, RejectsDifferentSdkCodedDimensions) {
  const auto dimensions = ParseQsvHevcSpsDimensions(PaddedSps());
  ASSERT_TRUE(dimensions);
  EXPECT_FALSE(QsvHevcDimensionsMatch(*dimensions, 32, 18, 32, 32, 30, 24));
  // The SDK HEVC field must match coded height, not the visible crop height.
  EXPECT_FALSE(QsvHevcDimensionsMatch(*dimensions, 32, 18, 32, 32, 32, 18));
}

TEST(QsvHevcDimensionsTest, RejectsStorageSmallerThanCodedPicture) {
  const auto dimensions = ParseQsvHevcSpsDimensions(PaddedSps());
  ASSERT_TRUE(dimensions);
  EXPECT_FALSE(QsvHevcDimensionsMatch(*dimensions, 32, 18, 16, 32, 32, 24));
  EXPECT_FALSE(QsvHevcDimensionsMatch(*dimensions, 32, 18, 32, 18, 32, 24));
}

TEST(QsvHevcDimensionsTest, RejectsMissingOrTruncatedSps) {
  EXPECT_FALSE(ParseQsvHevcSpsDimensions({}));
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(HexBytes("000000014401c173d089")));
  auto truncated = PaddedSps();
  truncated.resize(12);
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(truncated));
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(HexBytes("000000014201")));
}

TEST(QsvHevcDimensionsTest, RequiresExactlyOneAnnexBNal) {
  auto sps = PaddedSps();
  const auto duplicate = sps;
  sps.insert(sps.end(), duplicate.begin(), duplicate.end());
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
  sps = PaddedSps();
  EXPECT_FALSE(
      ParseQsvHevcSpsDimensions(ArrayView<const uint8_t>(sps).subview(4)));
  sps.insert(sps.begin(), 0xff);
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
  sps = PaddedSps();
  sps.erase(
      sps.begin());  // Both legal Annex B start-code lengths are accepted.
  EXPECT_TRUE(ParseQsvHevcSpsDimensions(sps));
}

TEST(QsvHevcDimensionsTest, RejectsForbiddenLayerAndTemporalHeaders) {
  auto sps = PaddedSps();
  sps[4] |= 0x80;
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
  sps = PaddedSps();
  sps[4] |= 1;
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
  sps = PaddedSps();
  sps[5] = 9;  // Nonzero nuh_layer_id.
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
  sps[5] = 2;  // A second temporal layer.
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
  sps[5] = 0;  // Reserved temporal_id_plus1.
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
}

TEST(QsvHevcDimensionsTest, RejectsNonEightBitChromaFromRealSpsMutation) {
  // Replace the real padded fixture's chroma-depth UE(0) with UE(1), preserving
  // the remaining RBSP bits and re-escaping them. The common parser accepts
  // its exposed fields; the extra prefix check must reject the chroma depth.
  const auto sps = HexBytes(
      "0000000142010101600000030090000003000003005da04219f25164aabcaf0168080000"
      "03000800000300f040");
  const auto parsed =
      H265SpsParser::ParseSps(ArrayView<const uint8_t>(sps).subview(6));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->bit_depth_luma_minus8, 0u);
  EXPECT_EQ(parsed->chroma_format_idc, 1u);
  EXPECT_EQ(parsed->width, 32u);
  EXPECT_EQ(parsed->height, 18u);
  EXPECT_FALSE(ParseQsvHevcSpsDimensions(sps));
}

}  // namespace
}  // namespace webrtc::external_encoder
