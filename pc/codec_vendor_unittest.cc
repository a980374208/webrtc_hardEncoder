/*
 *  Copyright 2004 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/codec_vendor.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/media_types.h"
#include "api/rtc_error.h"
#include "api/rtp_transceiver_direction.h"
#include "api/test/rtc_error_matchers.h"
#include "call/fake_payload_type_suggester.h"
#include "media/base/codec.h"
#include "media/base/codec_list.h"
#include "media/base/fake_media_engine.h"
#include "media/base/media_constants.h"
#include "media/base/test_utils.h"
#include "pc/media_options.h"
#include "pc/rtp_parameters_conversion.h"
#include "pc/session_description.h"
#include "rtc_base/checks.h"
#include "test/create_test_field_trials.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace webrtc {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::Ne;
using ::testing::Not;
using ::testing::Pair;

Codec CreateRedAudioCodec(absl::string_view encoding_id) {
  Codec red = CreateAudioCodec(63, "red", 48000, 2);
  red.SetParam(kCodecParamNotInNameValueFormat,
               std::string(encoding_id) + '/' + std::string(encoding_id));
  return red;
}

Codec CreateH264VideoCodec(int payload_type,
                           absl::string_view profile_level_id,
                           absl::string_view packetization_mode = "1",
                           absl::string_view level_asymmetry_allowed = "1") {
  Codec codec = CreateVideoCodec(payload_type, kH264CodecName);
  if (!profile_level_id.empty()) {
    codec.params[kH264FmtpProfileLevelId] = std::string(profile_level_id);
  }
  codec.params[kH264FmtpPacketizationMode] = std::string(packetization_mode);
  codec.params[kH264FmtpLevelAsymmetryAllowed] =
      std::string(level_asymmetry_allowed);
  return codec;
}

const Codec kAudioCodecs1[] = {CreateAudioCodec(111, "opus", 48000, 2),
                               CreateRedAudioCodec("111"),
                               CreateAudioCodec(102, "G722", 16000, 1),
                               CreateAudioCodec(0, "PCMU", 8000, 1),
                               CreateAudioCodec(8, "PCMA", 8000, 1),
                               CreateAudioCodec(107, "CN", 48000, 1)};

const Codec kAudioCodecs2[] = {
    CreateAudioCodec(126, "foo", 16000, 1),
    CreateAudioCodec(0, "PCMU", 8000, 1),
    CreateAudioCodec(127, "G722", 16000, 1),
};

const Codec kAudioCodecsAnswer[] = {
    CreateAudioCodec(102, "G722", 16000, 1),
    CreateAudioCodec(0, "PCMU", 8000, 1),
};

TEST(CodecVendorTest, TestSetAudioCodecs) {
  FieldTrials trials = CreateTestFieldTrials();
  std::vector<Codec> send_codecs = MAKE_VECTOR(kAudioCodecs1);
  std::vector<Codec> recv_codecs = MAKE_VECTOR(kAudioCodecs2);

  // The merged list of codecs should contain any send codecs that are also
  // nominally in the receive codecs list. Payload types should be picked from
  // the send codecs and a number-of-channels of 0 and 1 should be equivalent
  // (set to 1). This equals what happens when the send codecs are used in an
  // offer and the receive codecs are used in the following answer.
  const std::vector<Codec> sendrecv_codecs = MAKE_VECTOR(kAudioCodecsAnswer);
  RTC_CHECK_EQ(send_codecs[2].name, "G722")
      << "Please don't change shared test data!";
  RTC_CHECK_EQ(recv_codecs[2].name, "G722")
      << "Please don't change shared test data!";
  // Alter iLBC send codec to have zero channels, to test that that is handled
  // properly.
  send_codecs[2].channels = 0;

  // Alter PCMU receive codec to be lowercase, to test that case conversions
  // are handled properly.
  recv_codecs[1].name = "pcmu";

  // Test proper merge
  FakeMediaEngine media_engine;
  media_engine.SetAudioSendCodecs(send_codecs);
  media_engine.SetAudioRecvCodecs(recv_codecs);
  {
    CodecVendor codec_vendor(&media_engine, false, trials);
    EXPECT_EQ(send_codecs, codec_vendor.audio_send_codecs().codecs());
    EXPECT_EQ(recv_codecs, codec_vendor.audio_recv_codecs().codecs());
    EXPECT_EQ(sendrecv_codecs, codec_vendor.audio_sendrecv_codecs().codecs());
  }

  // Test empty send codecs list
  CodecList no_codecs;
  media_engine.SetAudioSendCodecs(no_codecs.codecs());
  media_engine.SetAudioRecvCodecs(recv_codecs);
  {
    CodecVendor codec_vendor(&media_engine, false, trials);
    EXPECT_EQ(no_codecs.codecs(), codec_vendor.audio_send_codecs().codecs());
    EXPECT_EQ(recv_codecs, codec_vendor.audio_recv_codecs().codecs());
    EXPECT_EQ(no_codecs.codecs(),
              codec_vendor.audio_sendrecv_codecs().codecs());
  }

  // Test empty recv codecs list
  media_engine.SetAudioSendCodecs(send_codecs);
  media_engine.SetAudioRecvCodecs(no_codecs.codecs());
  {
    CodecVendor codec_vendor(&media_engine, false, trials);
    EXPECT_EQ(send_codecs, codec_vendor.audio_send_codecs().codecs());
    EXPECT_EQ(no_codecs.codecs(), codec_vendor.audio_recv_codecs().codecs());
    EXPECT_EQ(no_codecs.codecs(),
              codec_vendor.audio_sendrecv_codecs().codecs());
  }

  // Test all empty codec lists
  media_engine.SetAudioSendCodecs(no_codecs.codecs());
  media_engine.SetAudioRecvCodecs(no_codecs.codecs());
  {
    CodecVendor codec_vendor(&media_engine, false, trials);
    EXPECT_EQ(no_codecs, codec_vendor.audio_send_codecs());
    EXPECT_EQ(no_codecs, codec_vendor.audio_recv_codecs());
    EXPECT_EQ(no_codecs, codec_vendor.audio_sendrecv_codecs());
  }
}

TEST(CodecVendorTest, VideoRtxIsIncludedWhenAskedFor) {
  Environment env = CreateEnvironment();
  FakeMediaEngine media_engine;
  std::vector<Codec> video_codecs({
      CreateVideoCodec(97, "vp8"),
      CreateVideoRtxCodec(98, 97),
  });
  media_engine.SetVideoSendCodecs(video_codecs);
  CodecVendor codec_vendor(&media_engine, /* rtx_enabled= */ true,
                           env.field_trials());
  FakePayloadTypeSuggester pt_suggester;
  RTCErrorOr<std::vector<Codec>> offered_codecs =
      codec_vendor.GetNegotiatedCodecsForOffer(
          MediaDescriptionOptions(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false),
          MediaSessionOptions(), nullptr, pt_suggester);
  EXPECT_THAT(offered_codecs.value(),
              Contains(Field("name", &Codec::name, "rtx")));
}

TEST(CodecVendorTest, VideoRtxIsExcludedWhenNotAskedFor) {
  Environment env = CreateEnvironment();
  FakeMediaEngine media_engine;
  std::vector<Codec> video_codecs({
      CreateVideoCodec(97, "vp8"),
      CreateVideoRtxCodec(98, 97),
  });
  media_engine.SetVideoSendCodecs(video_codecs);
  CodecVendor codec_vendor(&media_engine, /* rtx_enabled= */ false,
                           env.field_trials());
  FakePayloadTypeSuggester pt_suggester;
  RTCErrorOr<std::vector<Codec>> offered_codecs =
      codec_vendor.GetNegotiatedCodecsForOffer(
          MediaDescriptionOptions(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false),
          MediaSessionOptions(), nullptr, pt_suggester);
  EXPECT_THAT(offered_codecs.value(),
              Not(Contains(Field("name", &Codec::name, "rtx"))));
}

TEST(CodecVendorTest, PreferencesAffectCodecChoice) {
  Environment env = CreateEnvironment();
  FakeMediaEngine media_engine;
  std::vector<Codec> video_codecs({
      CreateVideoCodec(97, "vp8"),
      CreateVideoRtxCodec(98, 97),
      CreateVideoCodec(99, "vp9"),
      CreateVideoRtxCodec(100, 99),
  });
  media_engine.SetVideoSendCodecs(video_codecs);
  CodecVendor codec_vendor(&media_engine, /* rtx_enabled= */ false,
                           env.field_trials());
  MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false);
  options.codec_preferences = {
      ToRtpCodecCapability(CreateVideoCodec(-1, "vp9")),
  };
  FakePayloadTypeSuggester pt_suggester;

  RTCErrorOr<std::vector<Codec>> offered_codecs =
      codec_vendor.GetNegotiatedCodecsForOffer(options, MediaSessionOptions(),
                                               nullptr, pt_suggester);
  ASSERT_TRUE(offered_codecs.ok());
  EXPECT_THAT(offered_codecs.value(),
              Contains(Field("name", &Codec::name, "vp9")));
  EXPECT_THAT(offered_codecs.value(),
              Not(Contains(Field("name", &Codec::name, "vp8"))));
  EXPECT_THAT(offered_codecs.value().size(), Eq(1));
}

TEST(CodecVendorTest, H264OfferLevelsFollowMediaDirection) {
  Environment env = CreateEnvironment();
  const struct {
    const char* send_profile_level_id;
    const char* recv_profile_level_id;
    const char* level_asymmetry_allowed;
    RtpTransceiverDirection direction;
    const char* offered_profile_level_id;
  } cases[] = {
      {"420028", "42001f", "1", RtpTransceiverDirection::kSendOnly, "420028"},
      {"420028", "42001f", "1", RtpTransceiverDirection::kRecvOnly, "42001f"},
      {"42001f", "420028", "1", RtpTransceiverDirection::kRecvOnly, "420028"},
      {"420028", "42001f", "1", RtpTransceiverDirection::kSendRecv, "42001f"},
      {"42001f", "420028", "1", RtpTransceiverDirection::kSendRecv, "420028"},
      {"420028", "42001f", "0", RtpTransceiverDirection::kSendRecv, "42001f"},
      {"42001f", "420028", "0", RtpTransceiverDirection::kSendRecv, "42001f"},
      {"42e00b", "42f00b", "0", RtpTransceiverDirection::kSendRecv, "42f00b"},
      {"42001f", "420028", "1", RtpTransceiverDirection::kInactive, "420028"},
      {"42001f", "420028", "0", RtpTransceiverDirection::kStopped, "42001f"},
  };
  for (const auto& test_case : cases) {
    for (bool use_preferences : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "send=" << test_case.send_profile_level_id
                   << " recv=" << test_case.recv_profile_level_id
                   << " asymmetry=" << test_case.level_asymmetry_allowed
                   << " direction=" << static_cast<int>(test_case.direction)
                   << " preferences=" << use_preferences);
      Codec send_codec =
          CreateH264VideoCodec(97, test_case.send_profile_level_id, "1",
                               test_case.level_asymmetry_allowed);
      Codec recv_codec =
          CreateH264VideoCodec(101, test_case.recv_profile_level_id, "1",
                               test_case.level_asymmetry_allowed);
      FakeMediaEngine media_engine;
      media_engine.SetVideoSendCodecs({send_codec});
      media_engine.SetVideoRecvCodecs({recv_codec});
      CodecVendor codec_vendor(&media_engine, /*rtx_enabled=*/false,
                               env.field_trials());
      MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                      test_case.direction, false);
      if (use_preferences) {
        options.codec_preferences = {ToRtpCodecCapability(send_codec)};
      }
      FakePayloadTypeSuggester pt_suggester;
      auto offered_codecs = codec_vendor.GetNegotiatedCodecsForOffer(
          options, MediaSessionOptions(), nullptr, pt_suggester);
      ASSERT_TRUE(offered_codecs.ok());
      ASSERT_EQ(offered_codecs.value().size(), 1u);
      EXPECT_THAT(offered_codecs.value()[0].params,
                  Contains(Pair(kH264FmtpProfileLevelId,
                                test_case.offered_profile_level_id)));
    }
  }
}

TEST(CodecVendorTest, H264ReofferRestoresSendLevelAndPreservesPayloadTypes) {
  Environment env = CreateEnvironment();
  Codec send_codec = CreateH264VideoCodec(97, "420028");
  Codec recv_codec = CreateH264VideoCodec(101, "42001f");
  Codec current_codec = CreateH264VideoCodec(121, "42001f");
  current_codec.params["max-fs"] = "8160";
  const Codec current_rtx = CreateVideoRtxCodec(122, 121);
  auto current_description = std::make_unique<VideoContentDescription>();
  current_description->set_codecs({current_codec, current_rtx});
  ContentInfo current_content(MediaProtocolType::kRtp, "mid",
                              std::move(current_description));
  for (bool use_preferences : {false, true}) {
    SCOPED_TRACE(use_preferences);
    FakeMediaEngine media_engine;
    media_engine.SetVideoSendCodecs({send_codec, CreateVideoRtxCodec(98, 97)});
    media_engine.SetVideoRecvCodecs(
        {recv_codec, CreateVideoRtxCodec(102, 101)});
    CodecVendor codec_vendor(&media_engine, /*rtx_enabled=*/true,
                             env.field_trials());
    MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                    RtpTransceiverDirection::kSendOnly, false);
    if (use_preferences) {
      options.codec_preferences = {
          ToRtpCodecCapability(send_codec),
          ToRtpCodecCapability(CreateVideoRtxCodec(98, 97)),
      };
    }
    FakePayloadTypeSuggester pt_suggester;
    auto offered_codecs = codec_vendor.GetNegotiatedCodecsForOffer(
        options, MediaSessionOptions(), &current_content, pt_suggester);
    ASSERT_TRUE(offered_codecs.ok());
    Codec expected_codec = current_codec;
    expected_codec.params[kH264FmtpProfileLevelId] = "420028";
    EXPECT_THAT(offered_codecs.value(),
                ElementsAre(expected_codec, current_rtx));
  }
}

TEST(CodecVendorTest, H264OfferLevelsMatchProfileAndPacketizationMode) {
  Environment env = CreateEnvironment();
  const std::vector<Codec> send_codecs = {
      CreateH264VideoCodec(97, "420028", "1"),
      CreateH264VideoCodec(99, "42001f", "0"),
      CreateH264VideoCodec(101, "64002a", "1"),
  };
  const std::vector<Codec> recv_codecs = {
      CreateH264VideoCodec(107, "42001f", "1"),
      CreateH264VideoCodec(109, "420028", "0"),
      CreateH264VideoCodec(115, "64001f", "1"),
  };
  FakeMediaEngine media_engine;
  media_engine.SetVideoSendCodecs(send_codecs);
  media_engine.SetVideoRecvCodecs(recv_codecs);
  CodecVendor codec_vendor(&media_engine, /*rtx_enabled=*/false,
                           env.field_trials());
  MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false);
  for (const Codec& codec : send_codecs) {
    options.codec_preferences.push_back(ToRtpCodecCapability(codec));
  }
  FakePayloadTypeSuggester pt_suggester;
  auto offered_codecs = codec_vendor.GetNegotiatedCodecsForOffer(
      options, MediaSessionOptions(), nullptr, pt_suggester);
  ASSERT_TRUE(offered_codecs.ok());
  std::vector<Codec> expected_codecs = recv_codecs;
  for (size_t i = 0; i < expected_codecs.size(); ++i) {
    expected_codecs[i].params[kH264FmtpProfileLevelId] =
        send_codecs[i].params.at(kH264FmtpProfileLevelId);
  }
  EXPECT_EQ(offered_codecs.value(), expected_codecs);
}

TEST(CodecVendorTest, H264OfferPreservesDefaultProfileLevelId) {
  Environment env = CreateEnvironment();
  Codec send_codec = CreateH264VideoCodec(97, "");
  Codec recv_codec = CreateH264VideoCodec(101, "42e028");
  FakeMediaEngine media_engine;
  media_engine.SetVideoSendCodecs({send_codec});
  media_engine.SetVideoRecvCodecs({recv_codec});
  CodecVendor codec_vendor(&media_engine, /*rtx_enabled=*/false,
                           env.field_trials());
  MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false);
  options.codec_preferences = {ToRtpCodecCapability(send_codec)};
  FakePayloadTypeSuggester pt_suggester;
  auto offered_codecs = codec_vendor.GetNegotiatedCodecsForOffer(
      options, MediaSessionOptions(), nullptr, pt_suggester);
  ASSERT_TRUE(offered_codecs.ok());
  recv_codec.params.erase(kH264FmtpProfileLevelId);
  EXPECT_THAT(offered_codecs.value(), ElementsAre(recv_codec));
}

TEST(CodecVendorTest, H264AnswerStillHonorsRemoteLevelWithoutAsymmetry) {
  Environment env = CreateEnvironment();
  Codec send_codec = CreateH264VideoCodec(97, "420028", "1", "0");
  Codec remote_codec = CreateH264VideoCodec(101, "42001f", "1", "0");
  FakeMediaEngine media_engine;
  media_engine.SetVideoSendCodecs({send_codec});
  CodecVendor codec_vendor(&media_engine, /*rtx_enabled=*/false,
                           env.field_trials());
  MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false);
  FakePayloadTypeSuggester pt_suggester;
  auto answered_codecs = codec_vendor.GetNegotiatedCodecsForAnswer(
      options, MediaSessionOptions(), RtpTransceiverDirection::kRecvOnly,
      RtpTransceiverDirection::kSendOnly, nullptr, {remote_codec},
      pt_suggester);
  EXPECT_THAT(answered_codecs,
              IsRtcOkAndHolds(std::vector<Codec>{remote_codec}));
}

TEST(CodecVendorTest, GetNegotiatedCodecsForAnswerSimple) {
  Environment env = CreateEnvironment();
  FakeMediaEngine media_engine;
  std::vector<Codec> video_codecs({
      CreateVideoCodec(97, "vp8"),
      CreateVideoRtxCodec(98, 97),
      CreateVideoCodec(99, "vp9"),
      CreateVideoRtxCodec(100, 99),
  });
  media_engine.SetVideoSendCodecs(video_codecs);
  CodecVendor codec_vendor(&media_engine, /* rtx_enabled= */ true,
                           env.field_trials());
  MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false);
  FakePayloadTypeSuggester pt_suggester;
  ContentInfo* current_content = nullptr;
  RTCErrorOr<std::vector<Codec>> answered_codecs =
      codec_vendor.GetNegotiatedCodecsForAnswer(
          options, MediaSessionOptions(), RtpTransceiverDirection::kSendOnly,
          RtpTransceiverDirection::kSendOnly, current_content, video_codecs,
          pt_suggester);
  EXPECT_THAT(answered_codecs, IsRtcOkAndHolds(video_codecs));
}

TEST(CodecVendorTest, GetNegotiatedCodecsForAnswerWithCollision) {
  Environment env = CreateEnvironment();
  FakeMediaEngine media_engine;
  std::vector<Codec> video_codecs({
      CreateVideoCodec(97, "vp8"),
      CreateVideoCodec(99, "vp9"),
      CreateVideoCodec(101, "av1"),
  });
  std::vector<Codec> remote_codecs({
      CreateVideoCodec(97, "av1"),
      CreateVideoCodec(99, "vp9"),
  });
  media_engine.SetVideoSendCodecs(video_codecs);
  CodecVendor codec_vendor(&media_engine, /* rtx_enabled= */ false,
                           env.field_trials());
  MediaDescriptionOptions options(MediaType::VIDEO, "mid",
                                  RtpTransceiverDirection::kSendOnly, false);
  FakePayloadTypeSuggester pt_suggester;
  ContentInfo* current_content = nullptr;
  RTCErrorOr<std::vector<Codec>> answered_codecs =
      codec_vendor.GetNegotiatedCodecsForAnswer(
          options, MediaSessionOptions(), RtpTransceiverDirection::kSendOnly,
          RtpTransceiverDirection::kSendOnly, current_content, remote_codecs,
          pt_suggester);
  EXPECT_THAT(answered_codecs, IsRtcOkAndHolds(remote_codecs));
}

TEST(CodecVendorMergeTest, BasicTestSetup) {
  CodecList reference_codecs;
  const std::string mid = "mid";
  CodecList merged_codecs;
  FakePayloadTypeSuggester pt_suggester;
  RTCError error =
      MergeCodecsForTesting(reference_codecs, mid, merged_codecs, pt_suggester);
  EXPECT_TRUE(error.ok());
}

TEST(CodecVendorMergeTest, IdenticalListsMergeWithNoChange) {
  CodecList reference_codecs;
  const std::string mid = "mid";
  CodecList merged_codecs;
  FakePayloadTypeSuggester pt_suggester;
  Codec some_codec = CreateVideoCodec(97, "foo");
  auto pt_or_error = pt_suggester.SuggestPayloadType(mid, some_codec);
  ASSERT_THAT(pt_or_error.value(), Eq(97));
  reference_codecs.push_back(some_codec);
  merged_codecs.push_back(some_codec);
  RTCError error =
      MergeCodecsForTesting(reference_codecs, mid, merged_codecs, pt_suggester);
  EXPECT_TRUE(error.ok());
  EXPECT_THAT(merged_codecs.size(), Eq(1));
  EXPECT_THAT(merged_codecs[0].id, Eq(97));
}

TEST(CodecVendorMergeTest, MergeRenumbersAdditionalCodecs) {
  CodecList reference_codecs;
  const std::string mid = "mid";
  CodecList merged_codecs;
  FakePayloadTypeSuggester pt_suggester;
  Codec some_codec = CreateVideoCodec(97, "foo");
  auto pt_or_error = pt_suggester.SuggestPayloadType(mid, some_codec);
  ASSERT_THAT(pt_or_error.value(), Eq(97));
  merged_codecs.push_back(some_codec);
  // Use the same PT for a reference codec. This should be renumbered.
  Codec some_other_codec = CreateVideoCodec(97, "bar");
  reference_codecs.push_back(some_other_codec);
  RTCError error =
      MergeCodecsForTesting(reference_codecs, mid, merged_codecs, pt_suggester);
  EXPECT_TRUE(error.ok());
  EXPECT_THAT(merged_codecs.size(), Eq(2));
  // Both foo and bar should be present
  EXPECT_THAT(merged_codecs.codecs(),
              UnorderedElementsAre(Field("name", &Codec::name, "foo"),
                                   Field("name", &Codec::name, "bar")));
  // Foo should retain 97
  EXPECT_THAT(merged_codecs.codecs(),
              Contains(AllOf(Field("name", &Codec::name, "foo"),
                             Field("id", &Codec::id, 97))));
  // Bar should not have 97
  EXPECT_THAT(merged_codecs.codecs(),
              Contains(AllOf(Field("name", &Codec::name, "bar"),
                             Field("id", &Codec::id, Ne(97)))));
}

TEST(CodecVendorMergeTest, MergeRenumbersRedCodecArgument) {
  CodecList reference_codecs;
  const std::string mid = "mid";
  CodecList merged_codecs;
  FakePayloadTypeSuggester pt_suggester;
  Codec some_codec = CreateAudioCodec(100, "foo", 8000, 1);
  merged_codecs.push_back(some_codec);
  // Push into "reference" with a different ID
  some_codec.id = 102;
  reference_codecs.push_back(some_codec);
  Codec red_codec = CreateAudioCodec(101, "red", 8000, 1);
  ASSERT_EQ(red_codec.GetResiliencyType(), Codec::ResiliencyType::kRed);
  red_codec.params[kCodecParamNotInNameValueFormat] = "102/102";
  reference_codecs.push_back(red_codec);
  // Merging should add the RED codec with parameter 100/100
  RTCError error =
      MergeCodecsForTesting(reference_codecs, mid, merged_codecs, pt_suggester);
  EXPECT_TRUE(error.ok());
  EXPECT_THAT(merged_codecs.size(), Eq(2));
  EXPECT_THAT(
      merged_codecs.codecs(),
      Contains(AllOf(Field("name", &Codec::name, "red"),
                     Field("params", &Codec::params,
                           ElementsAre(Pair(kCodecParamNotInNameValueFormat,
                                            "100/100"))))));
}

TEST(CodecVendorMergeTest, MergeRenumbersRedCodecArgumentAndMerges) {
  CodecList reference_codecs;
  const std::string mid = "mid";
  CodecList merged_codecs;
  FakePayloadTypeSuggester pt_suggester;
  Codec some_codec = CreateAudioCodec(100, "foo", 8000, 1);
  merged_codecs.push_back(some_codec);
  // Push into "reference" with a different ID
  some_codec.id = 102;
  reference_codecs.push_back(some_codec);
  Codec red_codec = CreateAudioCodec(101, "red", 8000, 1);
  ASSERT_EQ(red_codec.GetResiliencyType(), Codec::ResiliencyType::kRed);
  red_codec.params[kCodecParamNotInNameValueFormat] = "102/102";
  reference_codecs.push_back(red_codec);
  // Push the same red codec into `merged_codecs` with the 100 id
  red_codec.params[kCodecParamNotInNameValueFormat] = "100/100";
  merged_codecs.push_back(red_codec);
  // Merging should note the duplication and not add another codec.
  RTCError error =
      MergeCodecsForTesting(reference_codecs, mid, merged_codecs, pt_suggester);
  EXPECT_TRUE(error.ok());
  EXPECT_THAT(merged_codecs.size(), Eq(2));
  EXPECT_THAT(
      merged_codecs.codecs(),
      Contains(AllOf(Field("name", &Codec::name, "red"),
                     Field("params", &Codec::params,
                           ElementsAre(Pair(kCodecParamNotInNameValueFormat,
                                            "100/100"))))));
}

TEST(CodecVendorMergeTest, MergeWithBrokenReferenceRedErrors) {
  CodecList reference_codecs;
  const std::string mid = "mid";
  CodecList merged_codecs;
  FakePayloadTypeSuggester pt_suggester;
  Codec some_codec = CreateAudioCodec(100, "foo", 8000, 1);
  Codec red_codec = CreateAudioCodec(101, "red", 8000, 1);
  // Adds a RED codec that refers to codec 102, which does not exist.
  red_codec.params[kCodecParamNotInNameValueFormat] = "100/102";
  reference_codecs.push_back(some_codec);
  reference_codecs.push_back(red_codec);
  // The bogus RED codec should result in an error return.
  RTCError error =
      MergeCodecsForTesting(reference_codecs, mid, merged_codecs, pt_suggester);
  EXPECT_FALSE(error.ok());
  EXPECT_THAT(error.type(), Eq(RTCErrorType::INTERNAL_ERROR));
}

}  // namespace
}  // namespace webrtc
