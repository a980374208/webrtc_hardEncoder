/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/nvenc_video_encoder_backend_linux.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include "modules/video_coding/codecs/external/linux_nvenc_driver.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"
#include "modules/video_coding/codecs/external/nvenc_capability_query.h"

namespace webrtc::external_encoder {
namespace {
constexpr size_t kBufferCount = 4;
constexpr int kMaximumDevices = 128;
constexpr int kMaximumDimension = 8192;

bool EqualGuid(const GUID& left, const GUID& right) {
  return std::memcmp(&left, &right, sizeof(left)) == 0;
}

bool ValidRates(const Rates& rates) {
  return rates.bitrate_bps > 0 && rates.framerate_fps > 0 &&
         rates.framerate_fps <= 240;
}

bool ValidLevel(uint8_t level) {
  constexpr uint8_t values[] = {10, 11, 12, 13, 20, 21, 22, 30, 31, 32,
                              40, 41, 42, 50, 51, 52, 60, 61, 62};
  return std::find(std::begin(values), std::end(values), level) != std::end(values);
}

void ApplyRates(const Rates& rates, NV_ENC_CONFIG* config) {
  config->rcParams.averageBitRate = rates.bitrate_bps;
  config->rcParams.maxBitRate = rates.bitrate_bps;
  config->rcParams.vbvBufferSize =
      std::max(1u, rates.bitrate_bps / rates.framerate_fps);
  config->rcParams.vbvInitialDelay = config->rcParams.vbvBufferSize;
}

struct Buffer {
  CUdeviceptr memory = 0;
  size_t pitch = 0;
  NV_ENC_REGISTERED_PTR registered = nullptr;
  NV_ENC_INPUT_PTR mapped = nullptr;
  NV_ENC_OUTPUT_PTR bitstream = nullptr;
  bool submitted = false;
  bool locked = false;
};

// A stable block is retained on unconfirmed retirement. Nothing in it may be
// released while a driver might still refer to a session, resource or DSO.
struct Resources {
  explicit Resources(std::shared_ptr<internal::LinuxNvencDriver> value)
      : driver(std::move(value)) {}
  std::shared_ptr<internal::LinuxNvencDriver> driver;
  CUcontext context = nullptr;
  CUdevice device = 0;
  void* session = nullptr;
  std::array<Buffer, kBufferCount> buffers;
  NV_ENC_CONFIG encode_config = {};
  NV_ENC_INITIALIZE_PARAMS initialize = {};
  bool initialized = false;
  bool encode_started = false;
};

class LinuxNvencBackend final : public VideoEncoderBackend {
 public:
  LinuxNvencBackend(int ordinal,
                    std::shared_ptr<internal::LinuxNvencDriver> injected = {},
                    std::optional<LinuxNvencDeviceSelection> expected = {},
                    std::shared_ptr<internal::LinuxNvencDeviceCollector> collector = {})
      : requested_ordinal_(ordinal), expected_(std::move(expected)),
        collector_(std::move(collector)), injected_driver_(std::move(injected)) {
    RecordLinuxEncoderBackendCreated(LinuxEncoderMetricBackend::kNvenc);
  }
  ~LinuxNvencBackend() override {
    Close();
    if (retirement_unconfirmed_) {
      // The global coordinator prevents future instances after this failure.
      // Preserve this instance's single bounded block until process exit.
      (void)resources_.release();
    }
  }

  Info GetInfo() const override { return {"NVENC-CUDA-Linux", true}; }

  Status Open(const Config& config) override {
    RecordLinuxEncoderBackendOpenAttempt(LinuxEncoderMetricBackend::kNvenc);
    Close();
    if (last_close_status_ != Status::kOk ||
        HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc) ==
            Status::kRetirementFailed) {
      return Fail(Status::kRetirementFailed, "nvenc_retirement_unconfirmed");
    }
    unhealthy_ = false;
    last_error_.clear();
    if (collector_) collector_->BeginOpen();
    if (expected_ && (expected_->cuda_ordinal < 0 || expected_->pci_bus_id.empty() ||
        expected_->pci_bus_id.size() >= 64 || expected_->pci_bus_id.find('\0') != std::string::npos))
      return Fail(Status::kInvalidArgument, "invalid_nvenc_device_selection");
    if (config.codec != Codec::kH264 || !ValidLevel(config.h264_level)) {
      return Fail(Status::kUnsupported, "h264_baseline_only");
    }
    if (requested_ordinal_ < -1 || config.width <= 0 || config.height <= 0 ||
        (config.width & 1) || (config.height & 1) ||
        config.width > kMaximumDimension || config.height > kMaximumDimension ||
        !ValidRates({config.bitrate_bps, config.framerate_fps})) {
      return Fail(Status::kInvalidArgument, "invalid_nvenc_configuration");
    }
    config_ = config;
    Status load_status = Status::kOk;
    auto driver = injected_driver_;
    if (!driver) driver = internal::LoadLinuxNvencDriver(&load_status, &last_error_);
    if (!driver) return load_status;
    if (!driver->HasRequiredFunctions()) {
      return Fail(Status::kUnavailable, "nvenc_function_table_incomplete");
    }
    constexpr uint32_t required =
        (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (driver->driver_api_version < required) {
      return Fail(Status::kUnsupported, "nvenc_api_version_unsupported");
    }
    if (driver->init(0) != CUDA_SUCCESS) {
      return Fail(Status::kUnavailable, "cuda_initialization_failed");
    }
    int count = 0;
    if (driver->device_get_count(&count) != CUDA_SUCCESS || count < 0 ||
        count > kMaximumDevices) {
      return Fail(Status::kUnavailable, "cuda_device_enumeration_failed");
    }
    struct Candidate {
      int ordinal;
      CUdevice device;
      std::array<char, 64> pci;
    };
    std::vector<Candidate> candidates;
    for (int ordinal = 0; ordinal < count; ++ordinal) {
      if (requested_ordinal_ >= 0 && ordinal != requested_ordinal_) continue;
      Candidate candidate{ordinal, 0, {}};
      if (driver->device_get(&candidate.device, ordinal) != CUDA_SUCCESS ||
          driver->device_get_pci_bus_id(candidate.pci.data(),
                                        static_cast<int>(candidate.pci.size()),
                                        candidate.device) != CUDA_SUCCESS ||
          !std::memchr(candidate.pci.data(), '\0', candidate.pci.size()) ||
          candidate.pci[0] == '\0') {
        continue;
      }
      if (expected_ && expected_->pci_bus_id != candidate.pci.data()) continue;
      candidates.push_back(candidate);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                const int order = std::strcmp(left.pci.data(), right.pci.data());
                return order != 0 ? order < 0 : left.ordinal < right.ordinal;
              });
    if (candidates.empty()) return Fail(Status::kUnavailable, "cuda_device_unavailable");
    Status candidate_status = Status::kUnavailable;
    for (const auto& candidate : candidates) {
      resources_ = std::make_unique<Resources>(driver);
      candidate_status = CreateContext(candidate.device);
      if (candidate_status == Status::kOk) {
        candidate_status = OnContext([&] { return ConfigureSession(); });
      }
      if (candidate_status == Status::kOk) {
        next_buffer_ = 0;
        frame_index_ = 0;
        last_error_.clear();
        if (collector_) collector_->Opened({candidate.ordinal, candidate.pci.data(), 0, driver->driver_api_version});
        RecordLinuxEncoderBackendOpenSucceeded(LinuxEncoderMetricBackend::kNvenc);
        return Status::kOk;
      }
      Close();
      if (last_close_status_ != Status::kOk) return last_close_status_;
    }
    return candidate_status;
  }

  Status SetRates(const Rates& rates) override {
    if (!Ready()) return Fail(Status::kError, "nvenc_session_not_ready");
    if (!ValidRates(rates)) return Fail(Status::kInvalidArgument, "invalid_nvenc_rates");
    if (config_.bitrate_bps == rates.bitrate_bps &&
        config_.framerate_fps == rates.framerate_fps) return Status::kOk;
    return OnContext([&] {
      auto& resource = *resources_;
      NV_ENC_CONFIG next = resource.encode_config;
      ApplyRates(rates, &next);
      NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
      reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
      reconfigure.reInitEncodeParams = resource.initialize;
      reconfigure.reInitEncodeParams.frameRateNum = rates.framerate_fps;
      reconfigure.reInitEncodeParams.encodeConfig = &next;
      reconfigure.forceIDR = 1;
      if (resource.driver->encode.nvEncReconfigureEncoder(resource.session,
                                                         &reconfigure) != NV_ENC_SUCCESS) {
        return Poison(Status::kError, "nvenc_reconfigure_failed");
      }
      resource.encode_config = next;
      resource.initialize.frameRateNum = rates.framerate_fps;
      config_.bitrate_bps = rates.bitrate_bps;
      config_.framerate_fps = rates.framerate_fps;
      return Status::kOk;
    });
  }

  Status Encode(const Frame& frame, bool force_key_frame, Packet* packet) override {
    if (packet) *packet = Packet{};
    if (!Ready()) return Fail(Status::kError, "nvenc_session_not_ready");
    if (!packet || !frame.y || !frame.uv || frame.width != config_.width ||
        frame.height != config_.height || frame.stride_y < frame.width ||
        frame.stride_uv < frame.width) {
      return Fail(Status::kInvalidArgument, "invalid_nvenc_frame");
    }
    const Status result = OnContext([&] { return EncodeOnContext(frame, force_key_frame, packet); });
    if (result != Status::kOk) *packet = Packet{};
    return result;
  }

  void Close() override {
    if (!resources_ || retirement_unconfirmed_) return;
    auto& resource = *resources_;
    if (!resource.context) {
      resources_.reset();
      return;
    }
    const Status retired = OnContext([&] { return RetireOnContext(); });
    if (retired != Status::kOk) {
      Preserve("nvenc_cleanup_failed");
      return;
    }
    if (resource.driver->context_destroy(resource.context) != CUDA_SUCCESS) {
      Preserve("cuda_context_destroy_failed");
      return;
    }
    resource.context = nullptr;
    resources_.reset();
    if (collector_) collector_->Closed();
  }

  Status LastCloseStatus() const override { return last_close_status_; }
  std::string_view LastError() const override { return last_error_; }

 private:
  bool Ready() const {
    return resources_ && resources_->initialized && !unhealthy_ &&
           !retirement_unconfirmed_;
  }
  Status Fail(Status status, const char* reason) {
    if (!retirement_unconfirmed_) last_error_ = reason;
    return status;
  }
  Status Poison(Status status, const char* reason) {
    unhealthy_ = true;
    return Fail(status, reason);
  }
  Status Preserve(const char* reason) {
    retirement_unconfirmed_ = true;
    if (collector_) collector_->FailedRetirement();
    unhealthy_ = true;
    last_close_status_ = Status::kRetirementFailed;
    last_error_ = reason;
    HardwareEncoderProbes().RecordRetirementFailure(HardwareEncoderVendor::kNvenc, reason);
    return Status::kRetirementFailed;
  }

  Status CreateContext(CUdevice device) {
    auto& resource = *resources_;
    CUcontext before = nullptr;
    if (resource.driver->context_get_current(&before) != CUDA_SUCCESS) {
      return Fail(Status::kError, "cuda_current_context_query_failed");
    }
    resource.device = device;
    const auto created = resource.driver->context_create(
        &resource.context, CU_CTX_SCHED_BLOCKING_SYNC, device);
    if (created != CUDA_SUCCESS) {
      if (resource.context) return Preserve("cuda_context_creation_unconfirmed");
      return Fail(Status::kUnavailable, "cuda_context_creation_failed");
    }
    if (!resource.context) return Preserve("cuda_context_missing");
    CUcontext popped = nullptr;
    CUcontext restored = nullptr;
    if (resource.driver->context_pop(&popped) != CUDA_SUCCESS ||
        popped != resource.context ||
        resource.driver->context_get_current(&restored) != CUDA_SUCCESS ||
        restored != before) {
      return Preserve("cuda_context_stack_unconfirmed");
    }
    return Status::kOk;
  }

  Status OnContext(const std::function<Status()>& operation) {
    auto& resource = *resources_;
    CUcontext before = nullptr;
    if (retirement_unconfirmed_) return Status::kRetirementFailed;
    if (resource.driver->context_get_current(&before) != CUDA_SUCCESS ||
        resource.driver->context_push(resource.context) != CUDA_SUCCESS) {
      return Preserve("cuda_context_push_failed");
    }
    CUdevice actual = 0;
    const bool device_ok = resource.driver->context_get_device(&actual) == CUDA_SUCCESS &&
                           actual == resource.device;
    // No device detail is sent to logs; the session is bound to this context.
    const Status result = device_ok ? operation() : Poison(Status::kError, "cuda_context_device_query_failed");
    CUcontext popped = nullptr;
    CUcontext restored = nullptr;
    if (resource.driver->context_pop(&popped) != CUDA_SUCCESS ||
        popped != resource.context ||
        resource.driver->context_get_current(&restored) != CUDA_SUCCESS ||
        restored != before) {
      return Preserve("cuda_context_stack_unconfirmed");
    }
    return result;
  }

  template <typename Value, typename Count, typename List, typename Predicate>
  Status RequireCapability(Count count, List list, Predicate matches) {
    std::vector<Value> values;
    const auto result = internal::QueryNvencCapabilities<Value>(count, list, &values);
    if (!result.ok()) return Fail(Status::kUnsupported, "nvenc_capability_query_failed");
    if (!std::any_of(values.begin(), values.end(), matches)) {
      return Fail(Status::kUnsupported, "nvenc_capability_unavailable");
    }
    return Status::kOk;
  }

  Status ConfigureSession() {
    auto& resource = *resources_;
    auto& api = resource.driver->encode;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open.device = resource.context;
    open.apiVersion = NVENCAPI_VERSION;
    if (api.nvEncOpenEncodeSessionEx(&open, &resource.session) != NV_ENC_SUCCESS) {
      return Fail(Status::kUnavailable, "nvenc_session_open_failed");
    }
    if (!resource.session) return Fail(Status::kError, "nvenc_session_missing");
    const GUID codec = NV_ENC_CODEC_H264_GUID;
    Status status = RequireCapability<GUID>(
        [&](uint32_t* count) { return api.nvEncGetEncodeGUIDCount(resource.session, count); },
        [&](GUID* values, uint32_t size, uint32_t* count) {
          return api.nvEncGetEncodeGUIDs(resource.session, values, size, count);
        }, [&](const GUID& value) { return EqualGuid(value, codec); });
    if (status != Status::kOk) return status;
    status = RequireCapability<GUID>(
        [&](uint32_t* count) { return api.nvEncGetEncodeProfileGUIDCount(resource.session, codec, count); },
        [&](GUID* values, uint32_t size, uint32_t* count) {
          return api.nvEncGetEncodeProfileGUIDs(resource.session, codec, values, size, count);
        }, [&](const GUID& value) { return EqualGuid(value, NV_ENC_H264_PROFILE_BASELINE_GUID); });
    if (status != Status::kOk) return status;
    status = RequireCapability<GUID>(
        [&](uint32_t* count) { return api.nvEncGetEncodePresetCount(resource.session, codec, count); },
        [&](GUID* values, uint32_t size, uint32_t* count) {
          return api.nvEncGetEncodePresetGUIDs(resource.session, codec, values, size, count);
        }, [&](const GUID& value) { return EqualGuid(value, NV_ENC_PRESET_P4_GUID); });
    if (status != Status::kOk) return status;
    status = RequireCapability<NV_ENC_BUFFER_FORMAT>(
        [&](uint32_t* count) { return api.nvEncGetInputFormatCount(resource.session, codec, count); },
        [&](NV_ENC_BUFFER_FORMAT* values, uint32_t size, uint32_t* count) {
          return api.nvEncGetInputFormats(resource.session, codec, values, size, count);
        }, [](NV_ENC_BUFFER_FORMAT value) { return value == NV_ENC_BUFFER_FORMAT_NV12; });
    if (status != Status::kOk) return status;
    int max_width = 0, max_height = 0, dynamic_bitrate = 0, modes = 0;
    int minimum_level = 0, maximum_level = 0;
    for (const auto& item : {std::pair{NV_ENC_CAPS_WIDTH_MAX, &max_width},
                             std::pair{NV_ENC_CAPS_HEIGHT_MAX, &max_height},
                             std::pair{NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE, &dynamic_bitrate},
                             std::pair{NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES, &modes},
                             std::pair{NV_ENC_CAPS_LEVEL_MIN, &minimum_level},
                             std::pair{NV_ENC_CAPS_LEVEL_MAX, &maximum_level}}) {
      NV_ENC_CAPS_PARAM caps = {};
      caps.version = NV_ENC_CAPS_PARAM_VER;
      caps.capsToQuery = item.first;
      if (api.nvEncGetEncodeCaps(resource.session, codec, &caps, item.second) != NV_ENC_SUCCESS) {
        return Fail(Status::kUnsupported, "nvenc_caps_query_failed");
      }
    }
    if (config_.width > max_width || config_.height > max_height || !dynamic_bitrate ||
        !(modes & NV_ENC_PARAMS_RC_CBR) || config_.h264_level < minimum_level ||
        config_.h264_level > maximum_level) {
      return Fail(Status::kUnsupported, "nvenc_configuration_unsupported");
    }
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (api.nvEncGetEncodePresetConfigEx(resource.session, codec, NV_ENC_PRESET_P4_GUID,
          NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) != NV_ENC_SUCCESS) {
      return Fail(Status::kUnsupported, "nvenc_preset_unavailable");
    }
    resource.encode_config = preset.presetCfg;
    auto& encode = resource.encode_config;
    encode.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    encode.gopLength = NVENC_INFINITE_GOPLENGTH;
    encode.frameIntervalP = 1;
    encode.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encode.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encode.rcParams.enableLookahead = 0;
    encode.rcParams.enableExtLookahead = 0;
    encode.rcParams.lookaheadDepth = 0;
    encode.rcParams.enableAQ = 0;
    encode.rcParams.enableTemporalAQ = 0;
    encode.rcParams.zeroReorderDelay = 1;
    encode.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    ApplyRates({config_.bitrate_bps, config_.framerate_fps}, &encode);
    auto& h264 = encode.encodeCodecConfig.h264Config;
    h264.enableTemporalSVC = 0;
    h264.enableStereoMVC = 0;
    h264.hierarchicalPFrames = 0;
    h264.hierarchicalBFrames = 0;
    h264.enableIntraRefresh = 0;
    h264.enableLTR = 0;
    h264.level = config_.h264_level;
    h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1;
    h264.disableSPSPPS = 0;
    h264.outputAUD = 0;
    h264.enableFillerDataInsertion = 0;
    h264.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
    h264.chromaFormatIDC = 1;
    h264.maxNumRefFrames = 1;
    h264.sliceMode = 3;
    h264.sliceModeData = 1;
    h264.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
    h264.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    h264.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    resource.initialize = {};
    auto& initialize = resource.initialize;
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = codec;
    initialize.presetGUID = NV_ENC_PRESET_P4_GUID;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initialize.encodeWidth = config_.width;
    initialize.encodeHeight = config_.height;
    initialize.darWidth = config_.width;
    initialize.darHeight = config_.height;
    initialize.frameRateNum = config_.framerate_fps;
    initialize.frameRateDen = 1;
    initialize.enablePTD = 1;
    initialize.enableEncodeAsync = 0;
    initialize.encodeConfig = &encode;
    if (api.nvEncInitializeEncoder(resource.session, &initialize) != NV_ENC_SUCCESS) {
      return Fail(Status::kUnsupported, "nvenc_initialize_failed");
    }
    resource.initialized = true;
    for (auto& buffer : resource.buffers) {
      status = AllocateBuffer(&buffer);
      if (status != Status::kOk) return status;
    }
    return Status::kOk;
  }

  Status AllocateBuffer(Buffer* buffer) {
    auto& resource = *resources_;
    auto& api = resource.driver->encode;
    if (resource.driver->memory_alloc_pitch(&buffer->memory, &buffer->pitch,
          static_cast<size_t>(config_.width), static_cast<size_t>(config_.height) * 3 / 2,
          16) != CUDA_SUCCESS) {
      return Fail(Status::kError, "cuda_surface_allocation_failed");
    }
    if (!buffer->memory || buffer->pitch < static_cast<size_t>(config_.width) ||
        buffer->pitch > std::numeric_limits<uint32_t>::max()) {
      return Fail(Status::kError, "cuda_surface_pitch_invalid");
    }
    NV_ENC_REGISTER_RESOURCE input = {};
    input.version = NV_ENC_REGISTER_RESOURCE_VER;
    input.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    input.resourceToRegister = reinterpret_cast<void*>(buffer->memory);
    input.width = config_.width;
    input.height = config_.height;
    input.pitch = static_cast<uint32_t>(buffer->pitch);
    input.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    input.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (api.nvEncRegisterResource(resource.session, &input) != NV_ENC_SUCCESS) {
      return Fail(Status::kError, "nvenc_surface_registration_failed");
    }
    buffer->registered = input.registeredResource;
    if (!buffer->registered) return Fail(Status::kError, "nvenc_registered_surface_missing");
    NV_ENC_CREATE_BITSTREAM_BUFFER output = {};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (api.nvEncCreateBitstreamBuffer(resource.session, &output) != NV_ENC_SUCCESS) {
      return Fail(Status::kError, "nvenc_bitstream_allocation_failed");
    }
    buffer->bitstream = output.bitstreamBuffer;
    if (!buffer->bitstream) return Fail(Status::kError, "nvenc_bitstream_missing");
    return Status::kOk;
  }

  Status EncodeOnContext(const Frame& frame, bool force_key_frame, Packet* packet) {
    auto& resource = *resources_;
    auto& api = resource.driver->encode;
    auto& buffer = resource.buffers[next_buffer_];
    CUDA_MEMCPY2D upload = {};
    upload.srcMemoryType = CU_MEMORYTYPE_HOST;
    upload.srcHost = frame.y;
    upload.srcPitch = static_cast<size_t>(frame.stride_y);
    upload.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    upload.dstDevice = buffer.memory;
    upload.dstPitch = buffer.pitch;
    upload.WidthInBytes = static_cast<size_t>(frame.width);
    upload.Height = static_cast<size_t>(frame.height);
    LinuxEncoderTransferTimer luma_upload(LinuxEncoderMetricTransfer::kCudaUpload,
        upload.WidthInBytes * upload.Height);
    const auto luma_status = resource.driver->copy_2d(&upload);
    luma_upload.Finish(luma_status == CUDA_SUCCESS);
    if (luma_status != CUDA_SUCCESS) {
      return Poison(Status::kError, "cuda_luma_upload_failed");
    }
    upload.srcHost = frame.uv;
    upload.srcPitch = static_cast<size_t>(frame.stride_uv);
    upload.dstY = static_cast<size_t>(frame.height);
    upload.Height = static_cast<size_t>(frame.height / 2);
    LinuxEncoderTransferTimer chroma_upload(LinuxEncoderMetricTransfer::kCudaUpload,
        upload.WidthInBytes * upload.Height);
    const auto chroma_status = resource.driver->copy_2d(&upload);
    chroma_upload.Finish(chroma_status == CUDA_SUCCESS);
    if (chroma_status != CUDA_SUCCESS) {
      return Poison(Status::kError, "cuda_chroma_upload_failed");
    }
    NV_ENC_MAP_INPUT_RESOURCE mapped = {};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = buffer.registered;
    if (api.nvEncMapInputResource(resource.session, &mapped) != NV_ENC_SUCCESS) {
      return Poison(Status::kError, "nvenc_input_map_failed");
    }
    buffer.mapped = mapped.mappedResource;
    if (!buffer.mapped || mapped.mappedBufferFmt != NV_ENC_BUFFER_FORMAT_NV12) {
      return Poison(Status::kError, "nvenc_mapped_format_invalid");
    }
    NV_ENC_PIC_PARAMS picture = {};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = frame.width;
    picture.inputHeight = frame.height;
    picture.inputPitch = static_cast<uint32_t>(buffer.pitch);
    picture.inputBuffer = buffer.mapped;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picture.outputBitstream = buffer.bitstream;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = frame.id;
    picture.frameIdx = frame_index_++;
    const bool requires_idr = force_key_frame || !resource.encode_started;
    if (requires_idr) picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    const NVENCSTATUS encode_status =
        api.nvEncEncodePicture(resource.session, &picture);
    if (encode_status == NV_ENC_ERR_ENCODER_BUSY) {
      // The API explicitly rejects this input when the encoder is busy. There
      // is no pending output to lock, including during Close. Keep the mapped
      // resource owned until Close unmaps it, but do not invent a submission.
      return Poison(Status::kError, "nvenc_encoder_busy");
    }
    resource.encode_started = true;
    // SUCCESS and NEED_MORE_INPUT accept input. For other undocumented errors,
    // retain the existing conservative retirement path rather than reuse it.
    buffer.submitted = true;
    if (encode_status != NV_ENC_SUCCESS) {
      return Poison(Status::kError, "nvenc_encode_failed");
    }
    NV_ENC_LOCK_BITSTREAM output = {};
    output.version = NV_ENC_LOCK_BITSTREAM_VER;
    output.outputBitstream = buffer.bitstream;
    output.doNotWait = 0;
    // This synchronous driver call has no deadline. No borrowed host storage is
    // retained after it returns; the caller must isolate waits using ownership.
    if (api.nvEncLockBitstream(resource.session, &output) != NV_ENC_SUCCESS) {
      return Poison(Status::kError, "nvenc_bitstream_lock_failed");
    }
    buffer.locked = true;
    buffer.submitted = false;
    const uint64_t maximum_size = static_cast<uint64_t>(frame.width) * frame.height * 4 + 65536;
    const bool valid = output.outputTimeStamp == frame.id && output.bitstreamBufferPtr &&
        output.bitstreamSizeInBytes > 0 && output.bitstreamSizeInBytes <= maximum_size &&
        (output.pictureType == NV_ENC_PIC_TYPE_IDR || output.pictureType == NV_ENC_PIC_TYPE_I ||
         output.pictureType == NV_ENC_PIC_TYPE_P) &&
        (!requires_idr || output.pictureType == NV_ENC_PIC_TYPE_IDR);
    if (valid) {
      const auto* bytes = static_cast<const uint8_t*>(output.bitstreamBufferPtr);
      packet->bytes.assign(bytes, bytes + output.bitstreamSizeInBytes);
      packet->frame_id = output.outputTimeStamp;
      packet->key_frame = output.pictureType == NV_ENC_PIC_TYPE_IDR;
      packet->qp = output.frameAvgQP <= 51 ? static_cast<int>(output.frameAvgQP) : -1;
    }
    if (api.nvEncUnlockBitstream(resource.session, buffer.bitstream) != NV_ENC_SUCCESS) {
      return Poison(Status::kError, "nvenc_bitstream_unlock_failed");
    }
    buffer.locked = false;
    if (api.nvEncUnmapInputResource(resource.session, buffer.mapped) != NV_ENC_SUCCESS) {
      return Poison(Status::kError, "nvenc_input_unmap_failed");
    }
    buffer.mapped = nullptr;
    if (!valid) return Poison(Status::kError, "nvenc_output_invalid");
    next_buffer_ = (next_buffer_ + 1) % kBufferCount;
    return Status::kOk;
  }

  Status RetireOnContext() {
    auto& resource = *resources_;
    auto& api = resource.driver->encode;
    if (resource.session && resource.initialized && resource.encode_started) {
      NV_ENC_PIC_PARAMS eos = {};
      eos.version = NV_ENC_PIC_PARAMS_VER;
      eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
      if (api.nvEncEncodePicture(resource.session, &eos) != NV_ENC_SUCCESS) {
        return Preserve("nvenc_eos_failed");
      }
    }
    for (auto& buffer : resource.buffers) {
      if (resource.session) {
        if (buffer.submitted && !buffer.locked) {
          NV_ENC_LOCK_BITSTREAM output = {};
          output.version = NV_ENC_LOCK_BITSTREAM_VER;
          output.outputBitstream = buffer.bitstream;
          if (api.nvEncLockBitstream(resource.session, &output) != NV_ENC_SUCCESS) {
            return Preserve("nvenc_pending_output_retirement_failed");
          }
          buffer.locked = true;
          buffer.submitted = false;
        }
        if (buffer.locked) {
          if (api.nvEncUnlockBitstream(resource.session, buffer.bitstream) != NV_ENC_SUCCESS) {
            return Preserve("nvenc_bitstream_unlock_retirement_failed");
          }
          buffer.locked = false;
        }
        if (buffer.mapped) {
          if (api.nvEncUnmapInputResource(resource.session, buffer.mapped) != NV_ENC_SUCCESS) {
            return Preserve("nvenc_input_unmap_retirement_failed");
          }
          buffer.mapped = nullptr;
        }
        if (buffer.registered) {
          if (api.nvEncUnregisterResource(resource.session, buffer.registered) != NV_ENC_SUCCESS) {
            return Preserve("nvenc_unregister_failed");
          }
          buffer.registered = nullptr;
        }
        if (buffer.bitstream) {
          if (api.nvEncDestroyBitstreamBuffer(resource.session, buffer.bitstream) != NV_ENC_SUCCESS) {
            return Preserve("nvenc_bitstream_destroy_failed");
          }
          buffer.bitstream = nullptr;
        }
      }
    }
    if (resource.session) {
      if (api.nvEncDestroyEncoder(resource.session) != NV_ENC_SUCCESS) {
        return Preserve("nvenc_session_destroy_failed");
      }
      resource.session = nullptr;
    }
    // CUDA storage may be released only after the encoder has retired every
    // registration and the session itself. Context/DSOs are released last.
    for (auto& buffer : resource.buffers) {
      if (buffer.memory) {
        if (resource.driver->memory_free(buffer.memory) != CUDA_SUCCESS) {
          return Preserve("cuda_surface_free_failed");
        }
        buffer.memory = 0;
      }
    }
    return Status::kOk;
  }

  const int requested_ordinal_;
  const std::optional<LinuxNvencDeviceSelection> expected_;
  const std::shared_ptr<internal::LinuxNvencDeviceCollector> collector_;
  std::shared_ptr<internal::LinuxNvencDriver> injected_driver_;
  std::unique_ptr<Resources> resources_;
  Config config_;
  std::string last_error_;
  Status last_close_status_ = Status::kOk;
  bool unhealthy_ = false;
  bool retirement_unconfirmed_ = false;
  size_t next_buffer_ = 0;
  uint32_t frame_index_ = 0;
};
}  // namespace

std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackend(int device_ordinal) {
  return std::make_unique<LinuxNvencBackend>(device_ordinal);
}

std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackend(
    const LinuxNvencDeviceSelection& selection) {
  return std::make_unique<LinuxNvencBackend>(selection.cuda_ordinal, nullptr, selection);
}
namespace internal {
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForProbe(
    int ordinal, std::shared_ptr<LinuxNvencDeviceCollector> collector) {
  return std::make_unique<LinuxNvencBackend>(ordinal, nullptr, std::nullopt, std::move(collector));
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForTesting(
    std::shared_ptr<LinuxNvencDriver> driver, int device_ordinal) {
  return std::make_unique<LinuxNvencBackend>(device_ordinal, std::move(driver));
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForTesting(
    std::shared_ptr<LinuxNvencDriver> driver, int ordinal,
    std::shared_ptr<LinuxNvencDeviceCollector> collector) {
  return std::make_unique<LinuxNvencBackend>(ordinal, std::move(driver), std::nullopt, std::move(collector));
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForTesting(
    std::shared_ptr<LinuxNvencDriver> driver, const LinuxNvencDeviceSelection& selection) {
  return std::make_unique<LinuxNvencBackend>(selection.cuda_ordinal, std::move(driver), selection);
}
}  // namespace internal
}  // namespace webrtc::external_encoder
