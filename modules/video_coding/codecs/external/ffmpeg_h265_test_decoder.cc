/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/ffmpeg_h265_test_decoder.h"

#include <windows.h>

// Compile this translation unit separately with the external OBS SDK's include
// root. Do not add a third_party/ffmpeg dependency or link avcodec.lib here.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
}

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

static_assert(LIBAVCODEC_VERSION_MAJOR == 61,
              "The test decoder requires the OBS avcodec-61 SDK headers");
static_assert(LIBAVUTIL_VERSION_MAJOR == 59,
              "The test decoder requires the OBS avutil-59 SDK headers");

namespace webrtc {
namespace {
constexpr size_t kMaximumAccessUnitBytes = 8 * 1024 * 1024;
constexpr size_t kMaximumPendingFrames = 32;

class FfmpegApi {
 public:
  ~FfmpegApi() {
    if (codec_module_)
      FreeLibrary(codec_module_);
    if (util_module_)
      FreeLibrary(util_module_);
  }

  bool Open(const std::string& directory, std::string* error) {
    if (ready_)
      return true;
    // A failed Configure may be retried; do not leak partially loaded modules.
    if (codec_module_) {
      FreeLibrary(codec_module_);
      codec_module_ = nullptr;
    }
    if (util_module_) {
      FreeLibrary(util_module_);
      util_module_ = nullptr;
    }
    const auto path = std::filesystem::u8path(directory);
    std::error_code filesystem_error;
    if (!path.is_absolute() ||
        !std::filesystem::is_directory(path, filesystem_error) ||
        filesystem_error) {
      *error = "FFmpeg DLL directory must exist and be absolute";
      return false;
    }
    // The dependent swresample/x264/zlib DLLs are resolved in this same
    // explicitly named directory. No process PATH or search directory changes.
    const DWORD flags =
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    util_module_ =
        LoadLibraryExW((path / "avutil-59.dll").c_str(), nullptr, flags);
    if (!util_module_) {
      *error = "LoadLibraryEx avutil-59.dll: " + std::to_string(GetLastError());
      return false;
    }
    codec_module_ =
        LoadLibraryExW((path / "avcodec-61.dll").c_str(), nullptr, flags);
    if (!codec_module_) {
      *error =
          "LoadLibraryEx avcodec-61.dll: " + std::to_string(GetLastError());
      return false;
    }
    if (!MatchesPath(util_module_, path / "avutil-59.dll") ||
        !MatchesPath(codec_module_, path / "avcodec-61.dll")) {
      *error = "FFmpeg DLL resolved to a different module path";
      return false;
    }
#define BIND_CODEC(name)                         \
  if (!Bind(codec_module_, #name, &name, error)) \
  return false
#define BIND_UTIL(name)                         \
  if (!Bind(util_module_, #name, &name, error)) \
  return false
    BIND_CODEC(avcodec_version);
    BIND_CODEC(avcodec_find_decoder_by_name);
    BIND_CODEC(avcodec_alloc_context3);
    BIND_CODEC(avcodec_open2);
    BIND_CODEC(avcodec_free_context);
    BIND_CODEC(avcodec_send_packet);
    BIND_CODEC(avcodec_receive_frame);
    BIND_CODEC(av_packet_alloc);
    BIND_CODEC(av_packet_free);
    BIND_CODEC(av_packet_unref);
    BIND_CODEC(av_new_packet);
    BIND_UTIL(avutil_version);
    BIND_UTIL(av_frame_alloc);
    BIND_UTIL(av_frame_free);
    BIND_UTIL(av_frame_unref);
    BIND_UTIL(av_strerror);
#undef BIND_CODEC
#undef BIND_UTIL
    if (avcodec_version() != LIBAVCODEC_VERSION_INT ||
        avutil_version() != LIBAVUTIL_VERSION_INT) {
      *error =
          "FFmpeg DLL versions differ from this test decoder's SDK headers";
      return false;
    }
    ready_ = true;
    return true;
  }

  std::string Describe(int code) const {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    if (!av_strerror || av_strerror(code, buffer, sizeof(buffer)) < 0)
      return std::to_string(code);
    return std::string(buffer) + " (" + std::to_string(code) + ")";
  }

#define API_FUNCTION(name) decltype(&::name) name = nullptr
  API_FUNCTION(avcodec_version);
  API_FUNCTION(avcodec_find_decoder_by_name);
  API_FUNCTION(avcodec_alloc_context3);
  API_FUNCTION(avcodec_open2);
  API_FUNCTION(avcodec_free_context);
  API_FUNCTION(avcodec_send_packet);
  API_FUNCTION(avcodec_receive_frame);
  API_FUNCTION(av_packet_alloc);
  API_FUNCTION(av_packet_free);
  API_FUNCTION(av_packet_unref);
  API_FUNCTION(av_new_packet);
  API_FUNCTION(avutil_version);
  API_FUNCTION(av_frame_alloc);
  API_FUNCTION(av_frame_free);
  API_FUNCTION(av_frame_unref);
  API_FUNCTION(av_strerror);
#undef API_FUNCTION

 private:
  template <typename Function>
  bool Bind(HMODULE module,
            const char* name,
            Function* function,
            std::string* error) {
    *function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (!*function)
      *error = "Missing FFmpeg export: " + std::string(name);
    return *function != nullptr;
  }

  bool MatchesPath(HMODULE module, const std::filesystem::path& expected) {
    std::wstring actual(32768, L'\0');
    const DWORD size = GetModuleFileNameW(module, actual.data(),
                                          static_cast<DWORD>(actual.size()));
    if (!size || size >= actual.size())
      return false;
    actual.resize(size);
    std::error_code error;
    return std::filesystem::equivalent(std::filesystem::path(actual), expected,
                                       error) &&
           !error;
  }

  HMODULE codec_module_ = nullptr, util_module_ = nullptr;
  bool ready_ = false;
};

AVPixelFormat SelectSoftware420(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE;
       ++format)
    if (*format == AV_PIX_FMT_YUV420P)
      return *format;
  return AV_PIX_FMT_NONE;
}

bool AnnexB(const EncodedImage& image) {
  return image.size() >= 6 && image.data() && image.data()[0] == 0 &&
         image.data()[1] == 0 &&
         (image.data()[2] == 1 ||
          (image.data()[2] == 0 && image.data()[3] == 1));
}
}  // namespace

class FfmpegH265TestDecoder::Impl {
 public:
  explicit Impl(std::string directory) : directory_(std::move(directory)) {}
  ~Impl() { Reset(); }

  bool Configure(const Settings& settings) {
    if (owner_ && *owner_ != GetCurrentThreadId())
      return Fail("Configure called on a different decoder sequence") == 0;
    owner_ = GetCurrentThreadId();
    Reset();
    failed_ = false;
    error_.clear();
    if (settings.codec_type() != kVideoCodecH265 ||
        settings.number_of_cores() <= 0)
      return Fail("Configure requires HEVC and a positive core count") == 0;
    const auto resolution = settings.max_render_resolution();
    maximum_width_ = resolution.Valid() ? resolution.Width() : 1920;
    maximum_height_ = resolution.Valid() ? resolution.Height() : 1080;
    if (maximum_width_ > 1920 || maximum_height_ > 1080)
      return Fail("Test decoder is bounded to 1920x1080") == 0;
    if (!api_.Open(directory_, &error_))
      return Fail(error_) == 0;
    const AVCodec* codec = api_.avcodec_find_decoder_by_name("hevc");
    if (!codec || codec->id != AV_CODEC_ID_HEVC ||
        codec->type != AVMEDIA_TYPE_VIDEO)
      return Fail("External FFmpeg software HEVC decoder is unavailable") == 0;
    context_ = api_.avcodec_alloc_context3(codec);
    frame_ = api_.av_frame_alloc();
    packet_ = api_.av_packet_alloc();
    if (!context_ || !frame_ || !packet_) {
      Fail("FFmpeg decoder allocation failed");
      Reset();
      return false;
    }
    context_->thread_count = 1;
    context_->thread_type = 0;
    context_->pkt_timebase = AVRational{1, 90000};
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context_->get_format = SelectSoftware420;
    // Bound allocation before the actual decoded-frame geometry is checked.
    // Coded HEVC dimensions may be rounded up to a coding-tree boundary.
    context_->max_pixels = static_cast<int64_t>((maximum_width_ + 63) & ~63) *
                           ((maximum_height_ + 63) & ~63);
    context_->err_recognition =
        AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE | AV_EF_CAREFUL;
    context_->error_concealment = 0;
    const int opened = api_.avcodec_open2(context_, codec, nullptr);
    if (opened < 0) {
      Fail("avcodec_open2: " + api_.Describe(opened));
      Reset();
      return false;
    }
    configured_ = true;
    return true;
  }

  int32_t Decode(const EncodedImage& image, int64_t render_time_ms) {
    (void)render_time_ms;
    if (!OnOwner() || !configured_ || failed_ || !callback_)
      return Fail("Decode requires a configured decoder, owner and callback");
    if (!AnnexB(image) || image.size() > kMaximumAccessUnitBytes ||
        image.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
      return Fail("Expected a bounded complete Annex-B HEVC access unit");
    if (pending_.size() >= kMaximumPendingFrames ||
        next_pts_ == std::numeric_limits<int64_t>::max())
      return Fail("HEVC decoder exceeded pending frame or PTS bound");
    for (const auto& entry : pending_)
      if (entry.second.rtp == image.RtpTimestamp())
        return Fail("Duplicate pending HEVC RTP timestamp");
    api_.av_packet_unref(packet_);
    const int allocated =
        api_.av_new_packet(packet_, static_cast<int>(image.size()));
    if (allocated < 0)
      return Fail("av_new_packet: " + api_.Describe(allocated));
    // av_new_packet allocates/zeros the required decoder overread padding.
    std::memcpy(packet_->data, image.data(), image.size());
    const int64_t pts = next_pts_++;
    packet_->pts = pts;
    packet_->dts =
        pts;  // This test path expects the product's no-B-frame mode.
    if (image._frameType == VideoFrameType::kVideoFrameKey)
      packet_->flags |= AV_PKT_FLAG_KEY;
    Metadata metadata;
    metadata.rtp = image.RtpTimestamp();
    metadata.ntp_ms = image.NtpTimeMs();
    metadata.width = image._encodedWidth;
    metadata.height = image._encodedHeight;
    metadata.rotation = image.rotation();
    int delivered = 0;
    int sent = api_.avcodec_send_packet(context_, packet_);
    if (sent == AVERROR(EAGAIN)) {
      if (!Receive(&delivered, false)) {
        api_.av_packet_unref(packet_);
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
      sent = api_.avcodec_send_packet(context_, packet_);
    }
    api_.av_packet_unref(packet_);
    if (sent < 0)
      return Fail("avcodec_send_packet: " + api_.Describe(sent));
    pending_.emplace(pts, metadata);
    if (!Receive(&delivered, false))
      return WEBRTC_VIDEO_CODEC_ERROR;
    return delivered ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  int32_t Register(DecodedImageCallback* callback) {
    if (owner_ && !OnOwner())
      return Fail(
          "Callback registration called on a different decoder sequence");
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() {
    if (owner_ && !OnOwner())
      return Fail("Release called on a different decoder sequence");
    if (configured_ && !failed_ && !pending_.empty()) {
      int delivered = 0;
      int result = api_.avcodec_send_packet(context_, nullptr);
      if (result == AVERROR(EAGAIN)) {
        if (Receive(&delivered, false))
          result = api_.avcodec_send_packet(context_, nullptr);
      }
      if (!failed_) {
        if (result < 0 && result != AVERROR_EOF)
          Fail("HEVC drain send: " + api_.Describe(result));
        else
          Receive(&delivered, true);
      }
      if (!failed_ && !pending_.empty())
        Fail("HEVC drain left unmatched submitted frame IDs");
    }
    const bool failed = failed_;
    Reset();
    callback_ = nullptr;
    owner_.reset();
    return failed ? WEBRTC_VIDEO_CODEC_ERROR : WEBRTC_VIDEO_CODEC_OK;
  }

  const std::string& error() const { return error_; }

 private:
  struct Metadata {
    uint32_t rtp = 0, width = 0, height = 0;
    int64_t ntp_ms = 0;
    VideoRotation rotation = kVideoRotation_0;
  };

  bool OnOwner() const { return owner_ && *owner_ == GetCurrentThreadId(); }

  int32_t Fail(std::string reason) {
    if (!failed_ || error_.empty())
      error_ = std::move(reason);
    failed_ = true;
    RTC_LOG(LS_ERROR) << "FFmpeg HEVC test decoder: " << error_;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  void Reset() {
    if (context_)
      api_.avcodec_free_context(&context_);
    if (packet_)
      api_.av_packet_free(&packet_);
    if (frame_)
      api_.av_frame_free(&frame_);
    pending_.clear();
    configured_ = false;
    next_pts_ = 1;
  }

  bool Receive(int* delivered, bool draining) {
    for (size_t count = 0; count <= kMaximumPendingFrames; ++count) {
      const int result = api_.avcodec_receive_frame(context_, frame_);
      if (result == AVERROR(EAGAIN)) {
        if (draining)
          Fail("HEVC drain unexpectedly requires new input");
        return !failed_;
      }
      if (result == AVERROR_EOF) {
        if (!draining)
          Fail("HEVC decoder reached EOF during active media");
        return !failed_;
      }
      if (result < 0) {
        Fail("avcodec_receive_frame: " + api_.Describe(result));
        return false;
      }
      const bool valid = Deliver();
      api_.av_frame_unref(frame_);
      if (!valid)
        return false;
      ++*delivered;
    }
    Fail("HEVC decoder produced too many frames from one receive loop");
    return false;
  }

  bool Deliver() {
    const auto found = pending_.find(frame_->pts);
    if (frame_->pts == AV_NOPTS_VALUE || found == pending_.end() || !callback_)
      return Fail("Decoded HEVC PTS does not match a submitted access unit") ==
             0;
    const Metadata metadata = found->second;
    if ((frame_->flags & AV_FRAME_FLAG_CORRUPT) || frame_->decode_error_flags ||
        frame_->format != AV_PIX_FMT_YUV420P ||
        context_->profile != AV_PROFILE_HEVC_MAIN)
      return Fail("Corrupt or non-Main/8-bit/4:2:0 HEVC output") == 0;
    if (frame_->width < 16 || frame_->height < 16 ||
        frame_->width > maximum_width_ || frame_->height > maximum_height_ ||
        frame_->width % 2 || frame_->height % 2 ||
        (metadata.width &&
         metadata.width != static_cast<uint32_t>(frame_->width)) ||
        (metadata.height &&
         metadata.height != static_cast<uint32_t>(frame_->height)))
      return Fail("Decoded HEVC geometry differs from the declared bounds") ==
             0;
    if (!frame_->data[0] || !frame_->data[1] || !frame_->data[2] ||
        frame_->linesize[0] < frame_->width ||
        frame_->linesize[1] < frame_->width / 2 ||
        frame_->linesize[2] < frame_->width / 2)
      return Fail("Decoded HEVC planes or strides are invalid") == 0;
    // Deep copy gives the native WebRTC sink independent ownership. FFmpeg's
    // frame is unreferenced immediately after this synchronous delivery.
    auto i420 = I420Buffer::Copy(frame_->width, frame_->height, frame_->data[0],
                                 frame_->linesize[0], frame_->data[1],
                                 frame_->linesize[1], frame_->data[2],
                                 frame_->linesize[2]);
    if (!i420)
      return Fail("I420 copy allocation failed") == 0;
    auto output = VideoFrame::Builder()
                      .set_video_frame_buffer(i420)
                      .set_rtp_timestamp(metadata.rtp)
                      .set_ntp_time_ms(metadata.ntp_ms)
                      .set_rotation(metadata.rotation)
                      .build();
    pending_.erase(found);
    // FFmpeg may retain a frame until a later access unit. An elapsed time from
    // submission would include that wait, so do not invent codec-only timing.
    callback_->Decoded(output, std::nullopt, std::nullopt);
    return true;
  }

  std::string directory_, error_;
  FfmpegApi api_;
  AVCodecContext* context_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  DecodedImageCallback* callback_ = nullptr;
  std::optional<DWORD> owner_;
  std::map<int64_t, Metadata> pending_;
  int64_t next_pts_ = 1;
  int maximum_width_ = 1920, maximum_height_ = 1080;
  bool configured_ = false, failed_ = false;
};

FfmpegH265TestDecoder::FfmpegH265TestDecoder(std::string absolute_dll_directory)
    : impl_(std::make_unique<Impl>(std::move(absolute_dll_directory))) {}
FfmpegH265TestDecoder::~FfmpegH265TestDecoder() = default;
bool FfmpegH265TestDecoder::Configure(const Settings& settings) {
  return impl_->Configure(settings);
}
int32_t FfmpegH265TestDecoder::Decode(const EncodedImage& image,
                                      int64_t render_time_ms) {
  return impl_->Decode(image, render_time_ms);
}
int32_t FfmpegH265TestDecoder::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  return impl_->Register(callback);
}
int32_t FfmpegH265TestDecoder::Release() {
  return impl_->Release();
}
VideoDecoder::DecoderInfo FfmpegH265TestDecoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = "FFmpeg HEVC test software / avcodec "
                            AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR) "."
                            AV_STRINGIFY(LIBAVCODEC_VERSION_MINOR) "."
                            AV_STRINGIFY(LIBAVCODEC_VERSION_MICRO) " / avutil "
                            AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR) "."
                            AV_STRINGIFY(LIBAVUTIL_VERSION_MINOR) "."
                            AV_STRINGIFY(LIBAVUTIL_VERSION_MICRO);
  info.is_hardware_accelerated = false;
  return info;
}
const std::string& FfmpegH265TestDecoder::last_error() const {
  return impl_->error();
}
}  // namespace webrtc
