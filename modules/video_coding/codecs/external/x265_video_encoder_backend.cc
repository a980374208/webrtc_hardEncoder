/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/x265_video_encoder_backend.h"

#include <x265.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "common_video/h265/h265_common.h"
#include "common_video/h265/h265_sps_parser.h"
#include "rtc_base/bitstream_reader.h"
#include "rtc_base/logging.h"

namespace webrtc::external_encoder {
namespace {

constexpr size_t kMaximumPacketBytes = 64 * 1024 * 1024;

bool ValidLevel(uint8_t level) {
  constexpr std::array<uint8_t, 13> kLevels = {
      30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
  return std::find(kLevels.begin(), kLevels.end(), level) != kLevels.end();
}

bool ValidRates(const Rates& rates) {
  return rates.bitrate_bps > 0 && rates.bitrate_bps <= 65535000 &&
         rates.framerate_fps > 0 && rates.framerate_fps <= 240;
}

bool ValidTuning(const X265Tuning& tuning) {
  return (tuning.wpp_pool_threads == 0 || tuning.wpp_pool_threads == 4 ||
          tuning.wpp_pool_threads == 8 || tuning.wpp_pool_threads == 16) &&
         (tuning.min_cu_size == 8 || tuning.min_cu_size == 16);
}

const char* PoolRequest(int threads) {
  switch (threads) {
    case 4:
      return "4";
    case 8:
      return "8";
    case 16:
      return "16";
    default:
      return "none";
  }
}

int Kilobits(uint32_t bitrate_bps) {
  return static_cast<int>((bitrate_bps + 999) / 1000);
}

bool HasNal(ArrayView<const uint8_t> bytes, H265::NaluType type) {
  for (const auto& nal : H265::FindNaluIndices(bytes)) {
    if (nal.payload_size >= 2 &&
        H265::ParseNaluType(bytes[nal.payload_start_offset]) == type) {
      return true;
    }
  }
  return false;
}

// Copy before any further x265 call: its NAL pointers belong to the encoder
// and may be invalidated by the next headers/encode operation.
bool CopyNals(const x265_nal* nals,
              uint32_t count,
              std::vector<uint8_t>* bytes) {
  bytes->clear();
  if (!nals || count == 0 || count > 1024) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const auto& nal = nals[i];
    if (!nal.payload || nal.sizeBytes == 0 ||
        nal.sizeBytes > kMaximumPacketBytes - bytes->size()) {
      return false;
    }
    const ArrayView<const uint8_t> payload(nal.payload, nal.sizeBytes);
    const auto indices = H265::FindNaluIndices(payload);
    if (indices.size() != 1 || indices[0].start_offset != 0 ||
        indices[0].payload_start_offset + indices[0].payload_size !=
            payload.size() ||
        indices[0].payload_size < 2) {
      return false;
    }
    const auto header = payload.subview(indices[0].payload_start_offset);
    // Layer 0, temporal_id_plus1=1, forbidden_zero_bit=0.
    if ((header[0] & 0x81) != 0 || header[1] != 1 ||
        static_cast<uint32_t>(H265::ParseNaluType(header[0])) != nal.type) {
      return false;
    }
    bytes->insert(bytes->end(), payload.begin(), payload.end());
  }
  return true;
}

bool ValidateSps(ArrayView<const uint8_t> bytes,
                 const Config& config,
                 const x265_param& actual) {
  int sps_count = 0;
  for (const auto& nal : H265::FindNaluIndices(bytes)) {
    const auto payload =
        bytes.subview(nal.payload_start_offset, nal.payload_size);
    if (H265::ParseNaluType(payload[0]) != H265::kSps) {
      continue;
    }
    ++sps_count;
    if (payload.size() <= 2 || payload[0] != 0x42 || payload[1] != 1) {
      return false;
    }
    const auto data = payload.subview(2);
    const auto parsed = H265SpsParser::ParseSps(data);
    const auto rbsp = H265::ParseRbsp(data);
    BitstreamReader reader(rbsp);
    reader.ConsumeBits(4);  // sps_video_parameter_set_id.
    const auto max_sub_layers = reader.ReadBits(3);
    reader.ConsumeBits(1);  // sps_temporal_id_nesting_flag.
    BitstreamReader tier_reader(rbsp);
    tier_reader.ConsumeBits(8);
    const auto profile_space = tier_reader.ReadBits(2);
    const bool high_tier = tier_reader.Read<bool>();
    const auto ptl = H265SpsParser::ParseProfileTierLevel(true, 0, reader);
    const bool reader_ok = reader.Ok();
    const bool tier_reader_ok = tier_reader.Ok();
    if (!reader_ok || !tier_reader_ok || max_sub_layers != 0 ||
        profile_space != 0 || high_tier || !ptl ||
        ptl->general_profile_idc != 1 ||
        !ValidLevel(static_cast<uint8_t>(ptl->general_level_idc)) ||
        ptl->general_level_idc > config.h265_level || !parsed ||
        parsed->chroma_format_idc != 1 ||
        parsed->sps_max_sub_layers_minus1 != 0 ||
        parsed->bit_depth_luma_minus8 != 0 ||
        parsed->width != static_cast<uint32_t>(config.width) ||
        parsed->height != static_cast<uint32_t>(config.height) ||
        parsed->pic_width_in_luma_samples !=
            static_cast<uint32_t>(actual.sourceWidth) ||
        parsed->pic_height_in_luma_samples !=
            static_cast<uint32_t>(actual.sourceHeight) ||
        parsed->width > parsed->pic_width_in_luma_samples ||
        parsed->height > parsed->pic_height_in_luma_samples) {
      return false;
    }
    // The common parser exposes luma bit depth, but not chroma bit depth.
    reader.ReadExponentialGolomb();  // sps_seq_parameter_set_id.
    reader.ReadExponentialGolomb();  // chroma_format_idc.
    reader.ReadExponentialGolomb();  // pic_width_in_luma_samples.
    reader.ReadExponentialGolomb();  // pic_height_in_luma_samples.
    if (reader.Read<bool>()) {
      for (int i = 0; i < 4; ++i) {
        reader.ReadExponentialGolomb();
      }
    }
    const auto luma_depth = reader.ReadExponentialGolomb();
    const auto chroma_depth = reader.ReadExponentialGolomb();
    if (!reader.Ok() || luma_depth != 0 || chroma_depth != 0) {
      return false;
    }
  }
  return sps_count == 1 && HasNal(bytes, H265::kVps) &&
         HasNal(bytes, H265::kPps);
}

class X265Backend final : public VideoEncoderBackend {
 public:
  explicit X265Backend(X265Tuning tuning) : tuning_(tuning) {}
  ~X265Backend() override { Close(); }

  Info GetInfo() const override { return {"x265 H265", false}; }
  std::string_view LastError() const override { return error_; }

  Status Open(const Config& config) override {
    Close();
    error_.clear();
    if (!ValidTuning(tuning_) || config.codec != Codec::kH265 ||
        config.width < 16 || config.height < 16 || config.width > 8192 ||
        config.height > 8192 || config.width % 2 || config.height % 2 ||
        !ValidLevel(config.h265_level) ||
        !ValidRates({config.bitrate_bps, config.framerate_fps})) {
      return Fail(Status::kInvalidArgument,
                  "invalid x265 tuning/codec/level/NV12 configuration");
    }
    config_ = config;
    api_ = x265_api_get(8);
    if (!api_ || api_->api_major_version != X265_MAJOR_VERSION ||
        api_->api_build_number != X265_BUILD || api_->bit_depth != 8 ||
        api_->sizeof_param != sizeof(x265_param) ||
        api_->sizeof_picture != sizeof(x265_picture) ||
        api_->sizeof_frame_stats != sizeof(x265_frame_stats)) {
      return Abort(Status::kUnavailable, "matching 8-bit x265 API unavailable");
    }
    parameters_ = api_->param_alloc();
    if (!parameters_) {
      return Abort(Status::kError, "x265 parameter allocation failed");
    }
    if (api_->param_default_preset(parameters_, "ultrafast", "zerolatency") <
        0) {
      return Abort(Status::kError, "x265 zerolatency preset unavailable");
    }
    parameters_->sourceWidth = config.width;
    parameters_->sourceHeight = config.height;
    parameters_->internalBitDepth = 8;
    parameters_->internalCsp = X265_CSP_I420;
    parameters_->fpsNum = config.framerate_fps;
    parameters_->fpsDenom = 1;
    parameters_->levelIdc = config.h265_level / 3;
    parameters_->bHighTier = 0;
    parameters_->bAllowNonConformance = 0;
    parameters_->frameNumThreads = 1;
    const char* pool = PoolRequest(tuning_.wpp_pool_threads);
    std::memcpy(parameters_->numaPools, pool, std::strlen(pool) + 1);
    parameters_->bEnableWavefront = tuning_.wpp_pool_threads != 0;
    parameters_->bDistributeModeAnalysis = 0;
    parameters_->bDistributeMotionEstimation = 0;
    parameters_->lookaheadDepth = 0;
    parameters_->lookaheadThreads = 0;
    parameters_->lookaheadSlices = 0;
    parameters_->bframes = 0;
    parameters_->bFrameAdaptive = 0;
    parameters_->bEnableTemporalSubLayers = 0;
    parameters_->bEnableTemporalFilter = 0;
    parameters_->numScalableLayers = 1;
    parameters_->bEnableSCC = 0;
    parameters_->bOpenGOP = 0;
    parameters_->scenecutThreshold = 0;
    parameters_->keyframeMax = 1000000;
    parameters_->keyframeMin = 1;
    parameters_->maxNumReferences = 1;
    parameters_->minCUSize = tuning_.min_cu_size;
    parameters_->bEnableSplitRdSkip = tuning_.split_rd_skip;
    parameters_->maxSlices = 1;
    parameters_->bCopyPicToFrame = 1;
    parameters_->bAnnexB = 1;
    parameters_->bRepeatHeaders = 1;
    parameters_->bEmitInfoSEI = 0;
    parameters_->bEmitHRDSEI = 0;
    parameters_->bEnablePsnr = 0;
    parameters_->bEnableSsim = 0;
    parameters_->logLevel = X265_LOG_ERROR;
    parameters_->rc.rateControlMode = X265_RC_ABR;
    parameters_->rc.bitrate = Kilobits(config.bitrate_bps);
    parameters_->rc.vbvMaxBitrate = parameters_->rc.bitrate;
    parameters_->rc.vbvBufferSize = parameters_->rc.bitrate;
    parameters_->rc.bStrictCbr = tuning_.strict_cbr;
    if (api_->param_apply_profile(parameters_, "main") < 0) {
      return Abort(Status::kUnsupported, "x265 Main profile unavailable");
    }
    encoder_ = api_->encoder_open(parameters_);
    if (!encoder_) {
      return Abort(Status::kUnsupported,
                   "x265 rejected dimensions/rates/negotiated Main level");
    }
    // x265 internally pads coded dimensions to minCUSize. Read its effective
    // configuration and bind those dimensions to SPS while checking display
    // dimensions against the caller's request.
    x265_param actual = {};
    // x265 4.1 copies extension fields only when that extension is compiled.
    // Follow its own default-then-copy contract; inactive single-layer fields
    // otherwise remain zero in a caller's readback structure.
    api_->param_default(&actual);
    api_->encoder_parameters(encoder_, &actual);
    if (!ValidateConfiguration(actual)) {
      return Abort(Status::kUnsupported,
                   "x265 changed required format/no-delay/rate configuration");
    }
    x265_nal* nals = nullptr;
    uint32_t count = 0;
    const int bytes = api_->encoder_headers(encoder_, &nals, &count);
    if (bytes <= 0 || !CopyNals(nals, count, &headers_) ||
        headers_.size() != static_cast<size_t>(bytes) ||
        !ValidateSps(headers_, config, actual)) {
      return Abort(Status::kUnsupported,
                   "x265 active SPS violates Main/level/display contract");
    }
    const size_t luma_bytes = static_cast<size_t>(config.width) * config.height;
    pixels_.resize(luma_bytes * 3 / 2);
    need_idr_ = true;
    return Status::kOk;
  }

  Status SetRates(const Rates& rates) override {
    if (!encoder_) {
      return Fail(Status::kError, "x265 encoder not initialized");
    }
    if (!ValidRates(rates)) {
      return Fail(Status::kInvalidArgument, "invalid x265 bitrate/framerate");
    }
    if (rates.bitrate_bps == config_.bitrate_bps &&
        rates.framerate_fps == config_.framerate_fps) {
      return Status::kOk;
    }
    // x265 4.1 Encoder::reconfigureParam copies rate-control fields but not
    // fpsNum/fpsDenom. Reopening handles both fields and regenerates
    // timing/SPS. Every successful Encode already retired its picture, so there
    // is no delayed frame to flush or discard. Caller frame IDs are never reset
    // here.
    Config config = config_;
    config.bitrate_bps = rates.bitrate_bps;
    config.framerate_fps = rates.framerate_fps;
    return Open(config);
  }

  Status Encode(const Frame& frame,
                bool force_key_frame,
                Packet* packet) override {
    if (packet) {
      *packet = {};
    }
    if (!encoder_) {
      return Fail(Status::kError, "x265 encoder not initialized");
    }
    if (!packet || !frame.y || !frame.uv || frame.width != config_.width ||
        frame.height != config_.height || frame.stride_y < frame.width ||
        frame.stride_uv < frame.width ||
        frame.id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return Fail(Status::kInvalidArgument, "invalid x265 input frame");
    }
    const size_t luma_bytes =
        static_cast<size_t>(config_.width) * config_.height;
    uint8_t* y_plane = pixels_.data();
    uint8_t* u_plane = y_plane + luma_bytes;
    uint8_t* v_plane = u_plane + luma_bytes / 4;
    for (int y = 0; y < frame.height; ++y) {
      std::memcpy(y_plane + static_cast<size_t>(y) * frame.width,
                  frame.y + static_cast<size_t>(y) * frame.stride_y,
                  frame.width);
    }
    for (int y = 0; y < frame.height / 2; ++y) {
      const uint8_t* source =
          frame.uv + static_cast<size_t>(y) * frame.stride_uv;
      const size_t row = static_cast<size_t>(y) * frame.width / 2;
      for (int x = 0; x < frame.width / 2; ++x) {
        u_plane[row + x] = source[x * 2];
        v_plane[row + x] = source[x * 2 + 1];
      }
    }
    x265_picture input = {}, output = {};
    api_->picture_init(parameters_, &input);
    api_->picture_init(parameters_, &output);
    input.pts = static_cast<int64_t>(frame.id);
    input.planes[0] = y_plane;
    input.planes[1] = u_plane;
    input.planes[2] = v_plane;
    input.stride[0] = frame.width;
    input.stride[1] = input.stride[2] = frame.width / 2;
    const bool request_idr = force_key_frame || need_idr_;
    input.sliceType = request_idr ? X265_TYPE_IDR : X265_TYPE_AUTO;
    x265_nal* nals = nullptr;
    uint32_t count = 0;
    const int encoded =
        api_->encoder_encode(encoder_, &nals, &count, &input, &output);
    // Zero output would violate the synchronous backend contract. Never retry
    // or flush under a different frame ID; close while owned input stays alive.
    std::vector<uint8_t> bytes;
    if (encoded != 1 || output.pts != input.pts || output.bitDepth != 8 ||
        output.colorSpace != X265_CSP_I420 || !CopyNals(nals, count, &bytes)) {
      return Abort(Status::kError,
                   "x265 expected one owned Annex B picture with matching PTS");
    }
    const bool idr =
        HasNal(bytes, H265::kIdrWRadl) || HasNal(bytes, H265::kIdrNLp);
    if ((!idr && !HasNal(bytes, H265::kTrailR) &&
         !HasNal(bytes, H265::kTrailN)) ||
        (request_idr && !idr)) {
      return Abort(Status::kError, "x265 did not emit requested IDR/picture");
    }
    if (idr && (!HasNal(bytes, H265::kVps) || !HasNal(bytes, H265::kSps) ||
                !HasNal(bytes, H265::kPps))) {
      if (headers_.size() > kMaximumPacketBytes - bytes.size()) {
        return Abort(Status::kError, "x265 access unit exceeds byte limit");
      }
      packet->bytes = headers_;
    }
    packet->bytes.insert(packet->bytes.end(), bytes.begin(), bytes.end());
    packet->frame_id = frame.id;
    packet->key_frame = idr;
    if (std::isfinite(output.frameData.qp)) {
      packet->qp =
          std::clamp(static_cast<int>(std::lround(output.frameData.qp)), 0, 51);
    }
    need_idr_ = false;
    error_.clear();
    return Status::kOk;
  }

  void Close() override {
    // Retire x265's frame thread before host buffers/parameters. This does not
    // call process-global x265_cleanup, which could affect other live encoders.
    if (encoder_) {
      api_->encoder_close(encoder_);
      encoder_ = nullptr;
    }
    if (parameters_) {
      api_->param_free(parameters_);
      parameters_ = nullptr;
    }
    headers_.clear();
    pixels_.clear();
    api_ = nullptr;
    need_idr_ = true;
  }

 private:
  bool ValidateConfiguration(const x265_param& actual) const {
    RTC_LOG(LS_INFO)
        << "X265_CONFIG coded=" << actual.sourceWidth << "x"
        << actual.sourceHeight << " depth=" << actual.internalBitDepth
        << " csp=" << actual.internalCsp << " fps=" << actual.fpsNum << "/"
        << actual.fpsDenom << " level=" << actual.levelIdc
        << " tier=" << actual.bHighTier
        << " nonconform=" << actual.bAllowNonConformance
        << " threads=" << actual.frameNumThreads
        << " wpp=" << actual.bEnableWavefront << " pools=" << actual.numaPools
        << " requestedPoolWorkers=" << tuning_.wpp_pool_threads
        << " distribute=" << actual.bDistributeModeAnalysis << "/"
        << actual.bDistributeMotionEstimation
        << " lookahead=" << actual.lookaheadDepth << "/"
        << actual.lookaheadThreads << " bframes=" << actual.bframes
        << " adaptive=" << actual.bFrameAdaptive
        << " temporal=" << actual.bEnableTemporalSubLayers << "/"
        << actual.bEnableTemporalFilter
        << " layers=" << actual.numScalableLayers
        << " scc=" << actual.bEnableSCC << " openGop=" << actual.bOpenGOP
        << " slices=" << actual.maxSlices << " minCU=" << actual.minCUSize
        << " splitRdSkip=" << actual.bEnableSplitRdSkip
        << " copy=" << actual.bCopyPicToFrame << " annexB=" << actual.bAnnexB
        << " headers=" << actual.bRepeatHeaders
        << " rc=" << actual.rc.rateControlMode << " rate=" << actual.rc.bitrate
        << " max=" << actual.rc.vbvMaxBitrate
        << " buffer=" << actual.rc.vbvBufferSize
        << " strictCbr=" << actual.rc.bStrictCbr;
    return actual.sourceWidth >= config_.width &&
           actual.sourceHeight >= config_.height &&
           actual.sourceWidth <= 8192 && actual.sourceHeight <= 8192 &&
           actual.internalBitDepth == 8 &&
           actual.internalCsp == X265_CSP_I420 &&
           actual.fpsNum == config_.framerate_fps && actual.fpsDenom == 1 &&
           actual.levelIdc == config_.h265_level / 3 && !actual.bHighTier &&
           !actual.bAllowNonConformance && actual.frameNumThreads == 1 &&
           actual.bEnableWavefront == (tuning_.wpp_pool_threads != 0) &&
           std::strcmp(actual.numaPools,
                       PoolRequest(tuning_.wpp_pool_threads)) == 0 &&
           !actual.bDistributeModeAnalysis &&
           !actual.bDistributeMotionEstimation && !actual.lookaheadDepth &&
           !actual.lookaheadThreads && !actual.bframes &&
           !actual.bFrameAdaptive && !actual.bEnableTemporalSubLayers &&
           !actual.bEnableTemporalFilter && actual.numScalableLayers == 1 &&
           !actual.bEnableSCC && !actual.bOpenGOP && actual.maxSlices == 1 &&
           actual.minCUSize == static_cast<uint32_t>(tuning_.min_cu_size) &&
           actual.bEnableSplitRdSkip == tuning_.split_rd_skip &&
           actual.bCopyPicToFrame && actual.bAnnexB && actual.bRepeatHeaders &&
           actual.rc.rateControlMode == X265_RC_ABR &&
           actual.rc.bitrate == Kilobits(config_.bitrate_bps) &&
           actual.rc.vbvMaxBitrate == Kilobits(config_.bitrate_bps) &&
           actual.rc.vbvBufferSize == Kilobits(config_.bitrate_bps) &&
           actual.rc.bStrictCbr == tuning_.strict_cbr;
  }

  Status Fail(Status status, std::string reason) {
    error_ = std::move(reason);
    return status;
  }
  Status Abort(Status status, std::string reason) {
    Close();
    return Fail(status, std::move(reason));
  }

  const X265Tuning tuning_;
  Config config_;
  const x265_api* api_ = nullptr;
  x265_param* parameters_ = nullptr;
  x265_encoder* encoder_ = nullptr;
  std::vector<uint8_t> pixels_;
  std::vector<uint8_t> headers_;
  std::string error_;
  bool need_idr_ = true;
};

}  // namespace

std::optional<X265Tuning> ParseX265ExperimentalTuning(std::string_view name) {
  if (name.empty() || name == "baseline") {
    return X265Tuning{};
  }
  if (name == "wpp4") {
    return X265Tuning{4, true, 8};
  }
  if (name == "wpp8") {
    return X265Tuning{8, true, 8};
  }
  if (name == "wpp16") {
    return X265Tuning{16, true, 8};
  }
  if (name == "wpp8-nostrict") {
    return X265Tuning{8, false, 8};
  }
  if (name == "baseline-cu16") {
    return X265Tuning{0, true, 16};
  }
  if (name == "wpp8-cu16") {
    return X265Tuning{8, true, 16};
  }
  if (name == "wpp8-rdskip") {
    return X265Tuning{8, true, 8, true};
  }
  return std::nullopt;
}

std::unique_ptr<VideoEncoderBackend> CreateX265H265Backend(X265Tuning tuning) {
  return std::make_unique<X265Backend>(tuning);
}

Status ProbeX265H265Backend(std::string* reason, uint8_t h265_level) {
  auto backend = CreateX265H265Backend();
  Config config;
  config.codec = Codec::kH265;
  config.width = 160;
  config.height = 128;
  config.bitrate_bps = 64000;
  config.framerate_fps = 15;
  config.h265_level = h265_level;
  const Status status = backend->Open(config);
  if (reason) {
    *reason = status == Status::kOk ? std::string()
                                    : std::string(backend->LastError());
  }
  return status;
}

}  // namespace webrtc::external_encoder
