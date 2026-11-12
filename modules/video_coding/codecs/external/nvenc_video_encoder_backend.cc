/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 * Use of this source code is governed by a BSD-style license
 * that can be found in the LICENSE file in the root of the source tree.
 */

#include "modules/video_coding/codecs/external/nvenc_video_encoder_backend.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include "modules/video_coding/codecs/external/nvenc_capability_query.h"
#include "nvEncodeAPI.h"
#include "rtc_base/logging.h"

namespace webrtc::external_encoder {
namespace {

using Microsoft::WRL::ComPtr;
constexpr uint64_t kStagingMapTimeoutMs = 200;
// NVENC requires at least four input buffers with no B frames. Only one is
// submitted at a time: the first integration deliberately avoids a frame queue.
constexpr size_t kBufferCount = 4;
constexpr uint32_t kNvidiaVendorId = 0x10de;
static_assert(sizeof(GUID) == 16);

std::string Hex(uint32_t value) {
  char text[11];
  std::snprintf(text, sizeof(text), "0x%08lx",
                static_cast<unsigned long>(value));
  return text;
}

const char* NvencErrorName(NVENCSTATUS status) {
  switch (status) {
    case NV_ENC_SUCCESS:
      return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
      return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
      return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:
      return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:
      return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:
      return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR:
      return "NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT:
      return "NV_ENC_ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM:
      return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:
      return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY:
      return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
      return "NV_ENC_ERR_ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
      return "NV_ENC_ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY:
      return "NV_ENC_ERR_LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:
      return "NV_ENC_ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION:
      return "NV_ENC_ERR_INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED:
      return "NV_ENC_ERR_MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT:
      return "NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:
      return "NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD:
      return "NV_ENC_ERR_EVENT_NOT_REGISTERD";
    case NV_ENC_ERR_GENERIC:
      return "NV_ENC_ERR_GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
      return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED:
      return "NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:
      return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:
      return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED:
      return "NV_ENC_ERR_RESOURCE_NOT_MAPPED";
    case NV_ENC_ERR_NEED_MORE_OUTPUT:
      return "NV_ENC_ERR_NEED_MORE_OUTPUT";
  }
  return "NV_ENC_ERR_UNKNOWN";
}

std::string FormatGuid(const GUID& guid) {
  char text[37];
  std::snprintf(text, sizeof(text),
                "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned long>(guid.Data1),
                static_cast<unsigned int>(guid.Data2),
                static_cast<unsigned int>(guid.Data3),
                static_cast<unsigned int>(guid.Data4[0]),
                static_cast<unsigned int>(guid.Data4[1]),
                static_cast<unsigned int>(guid.Data4[2]),
                static_cast<unsigned int>(guid.Data4[3]),
                static_cast<unsigned int>(guid.Data4[4]),
                static_cast<unsigned int>(guid.Data4[5]),
                static_cast<unsigned int>(guid.Data4[6]),
                static_cast<unsigned int>(guid.Data4[7]));
  return text;
}

std::string AdapterName(const WCHAR* description) {
  char text[512];
  if (!WideCharToMultiByte(CP_UTF8, 0, description, -1, text,
                           static_cast<int>(sizeof(text)), nullptr, nullptr)) {
    return "<unavailable>";
  }
  return text;
}

const char* QueryErrorName(internal::CapabilityQueryError error) {
  switch (error) {
    case internal::CapabilityQueryError::kNone:
      return "none";
    case internal::CapabilityQueryError::kCountCallFailed:
      return "count_call_failed";
    case internal::CapabilityQueryError::kEmptyCount:
      return "empty_count";
    case internal::CapabilityQueryError::kExcessiveCount:
      return "excessive_count";
    case internal::CapabilityQueryError::kListCallFailed:
      return "list_call_failed";
    case internal::CapabilityQueryError::kInvalidFilledCount:
      return "invalid_filled_count";
  }
  return "unknown";
}

void LogQuery(const char* kind, const internal::CapabilityQueryResult& result) {
  RTC_LOG(LS_INFO) << "NVENC " << kind
                   << " query error=" << QueryErrorName(result.error)
                   << " queried_count=" << result.queried_count
                   << " filled_count=" << result.filled_count
                   << " count_status=" << Hex(result.count_status)
                   << " list_status="
                   << (result.list_status ? Hex(*result.list_status)
                                          : "not_called");
}

void LogGuids(const char* kind, const std::vector<GUID>& guids) {
  for (size_t index = 0; index < guids.size(); ++index) {
    RTC_LOG(LS_INFO) << "NVENC " << kind << "[" << index
                     << "]=" << FormatGuid(guids[index]);
  }
}

bool EqualGuid(const GUID& a, const GUID& b) {
  return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

bool AllZeroGuids(const std::vector<GUID>& guids) {
  return !guids.empty() &&
         std::all_of(guids.begin(), guids.end(), [](const GUID& guid) {
           const GUID zero = {};
           return EqualGuid(guid, zero);
         });
}

bool ValidH264Level(uint8_t level) {
  constexpr uint8_t levels[] = {10, 11, 12, 13, 20, 21, 22, 30, 31, 32,
                                40, 41, 42, 50, 51, 52, 60, 61, 62};
  return std::find(std::begin(levels), std::end(levels), level) !=
         std::end(levels);
}

bool ValidH265Level(uint8_t level) {
  constexpr uint8_t levels[] = {30,  60,  63,  90,  93,  120, 123,
                                150, 153, 156, 180, 183, 186};
  return std::find(std::begin(levels), std::end(levels), level) !=
         std::end(levels);
}

bool ValidRates(uint32_t bitrate, uint32_t framerate) {
  return bitrate > 0 && framerate > 0 && framerate <= 240;
}

void ApplyRates(const Rates& rates, NV_ENC_CONFIG* config) {
  config->rcParams.averageBitRate = rates.bitrate_bps;
  config->rcParams.maxBitRate = rates.bitrate_bps;
  config->rcParams.vbvBufferSize =
      std::max(1u, rates.bitrate_bps / rates.framerate_fps);
  config->rcParams.vbvInitialDelay = config->rcParams.vbvBufferSize;
}

struct Buffer {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11Texture2D> staging;
  NV_ENC_REGISTERED_PTR registered = nullptr;
  NV_ENC_INPUT_PTR mapped = nullptr;
  NV_ENC_OUTPUT_PTR bitstream = nullptr;
  bool submitted = false;
  bool locked = false;
};

// Keep SDK-dependent objects at stable addresses until the session is known to
// be destroyed. At most one block is allocated for each backend instance.
struct NvencSessionResources {
  HMODULE library = nullptr;
  NV_ENCODE_API_FUNCTION_LIST api = {};
  void* session = nullptr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  std::array<Buffer, kBufferCount> buffers;
  NV_ENC_CONFIG encode_config = {};
  NV_ENC_INITIALIZE_PARAMS initialize = {};
};

class NvencBackend final : public VideoEncoderBackend {
 public:
  explicit NvencBackend(Codec codec) : codec_(codec) {}
  ~NvencBackend() override {
    Close();
    if (retirement_unconfirmed_) {
      // A failed DestroyEncoder gives no ownership guarantee. Preserve this
      // instance's one bounded block, including DLL and COM references, until
      // process exit. Moving/freeing it here could expose driver work to UAF.
      (void)resources_.release();
    }
  }

  Info GetInfo() const override {
    return {
        codec_ == Codec::kH264 ? "NVENC H264 / D3D11" : "NVENC H265 / D3D11",
        true};
  }

  Status Open(const Config& config) override {
    Close();
    std::string retirement_reason;
    if (HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc,
                                      &retirement_reason) ==
        Status::kRetirementFailed) {
      return Fail(Status::kRetirementFailed, retirement_reason.c_str());
    }
    if (last_close_status_ != Status::kOk) {
      return last_close_status_;
    }
    last_error_.clear();
    poisoned_ = false;
    if (config.codec != codec_ ||
        (codec_ == Codec::kH264 ? !ValidH264Level(config.h264_level)
                                : !ValidH265Level(config.h265_level))) {
      return Fail(Status::kUnsupported,
                  codec_ == Codec::kH264
                      ? "Only H.264 Baseline is supported"
                      : "Only HEVC Main with a supported level is supported");
    }
    if (config.width <= 0 || config.height <= 0 || (config.width & 1) ||
        (config.height & 1) ||
        !ValidRates(config.bitrate_bps, config.framerate_fps)) {
      return Fail(Status::kInvalidArgument, "Invalid NV12 dimensions or rates");
    }
    config_ = config;
    Status status = LoadApi();
    if (status != Status::kOk)
      return status;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr))
      return Fail(Status::kUnavailable, "CreateDXGIFactory1", hr);

    bool found_nvidia = false;
    Status candidate_status = Status::kUnavailable;
    for (UINT index = 0;; ++index) {
      if (last_close_status_ != Status::kOk) {
        return last_close_status_;
      }
      ComPtr<IDXGIAdapter1> adapter;
      hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(hr))
        return Fail(Status::kError, "EnumAdapters1", hr);
      DXGI_ADAPTER_DESC1 description = {};
      hr = adapter->GetDesc1(&description);
      if (FAILED(hr) || description.VendorId != kNvidiaVendorId ||
          (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        continue;
      }
      found_nvidia = true;
      RTC_LOG(LS_INFO) << "NVENC candidate adapter index=" << index
                       << " name=" << AdapterName(description.Description)
                       << " vendor=" << Hex(description.VendorId)
                       << " device=" << Hex(description.DeviceId);
      constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
      hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                             levels, 1, D3D11_SDK_VERSION,
                             device_.GetAddressOf(), nullptr,
                             context_.GetAddressOf());
      if (FAILED(hr)) {
        candidate_status = Fail(Status::kUnavailable, "D3D11CreateDevice", hr);
        CloseSession();
        continue;
      }
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> actual_adapter;
      DXGI_ADAPTER_DESC actual_description = {};
      hr = device_.As(&dxgi_device);
      if (SUCCEEDED(hr))
        hr = dxgi_device->GetAdapter(actual_adapter.GetAddressOf());
      if (SUCCEEDED(hr))
        hr = actual_adapter->GetDesc(&actual_description);
      if (FAILED(hr)) {
        candidate_status = Fail(Status::kError, "Inspect D3D11 adapter", hr);
        CloseSession();
        continue;
      }
      RTC_LOG(LS_INFO) << "NVENC actual D3D11 adapter name="
                       << AdapterName(actual_description.Description)
                       << " vendor=" << Hex(actual_description.VendorId)
                       << " device=" << Hex(actual_description.DeviceId);
      if (actual_description.VendorId != kNvidiaVendorId) {
        candidate_status =
            Fail(Status::kUnavailable, "D3D11 device is not an NVIDIA adapter");
        CloseSession();
        continue;
      }
      NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
      open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
      open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
      open.device = device_.Get();
      open.apiVersion = NVENCAPI_VERSION;
      NVENCSTATUS nv_status = api_.nvEncOpenEncodeSessionEx(&open, &session_);
      if (nv_status != NV_ENC_SUCCESS) {
        candidate_status =
            Fail(Status::kUnavailable, "NvEncOpenEncodeSessionEx", nv_status);
        CloseSession();
        continue;
      }
      if (!session_) {
        candidate_status =
            Fail(Status::kError,
                 "NvEncOpenEncodeSessionEx returned success without a session");
        CloseSession();
        continue;
      }
      candidate_status = ConfigureSession();
      if (candidate_status == Status::kOk) {
        last_error_.clear();
        return Status::kOk;
      }
      CloseSession();
    }
    if (!found_nvidia)
      return Fail(Status::kUnavailable, "No NVIDIA D3D11 adapter");
    return candidate_status;
  }

  Status SetRates(const Rates& rates) override {
    if (poisoned_)
      return Status::kError;
    if (!initialized_)
      return Fail(Status::kError, "NVENC session is closed");
    if (!ValidRates(rates.bitrate_bps, rates.framerate_fps))
      return Fail(Status::kInvalidArgument, "Invalid NVENC rates");
    if (rates.bitrate_bps == config_.bitrate_bps &&
        rates.framerate_fps == config_.framerate_fps) {
      return Status::kOk;
    }
    NV_ENC_CONFIG next_config = encode_config_;
    ApplyRates(rates, &next_config);
    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfigure.reInitEncodeParams = initialize_;
    reconfigure.reInitEncodeParams.frameRateNum = rates.framerate_fps;
    reconfigure.reInitEncodeParams.encodeConfig = &next_config;
    NVENCSTATUS status = api_.nvEncReconfigureEncoder(session_, &reconfigure);
    if (status != NV_ENC_SUCCESS)
      return Poison(Status::kError, "NvEncReconfigureEncoder", status);
    encode_config_ = next_config;
    initialize_.frameRateNum = rates.framerate_fps;
    config_.bitrate_bps = rates.bitrate_bps;
    config_.framerate_fps = rates.framerate_fps;
    return Status::kOk;
  }

  Status Encode(const Frame& frame,
                bool force_key_frame,
                Packet* packet) override {
    if (packet)
      *packet = Packet{};
    if (poisoned_)
      return Status::kError;
    if (!initialized_)
      return Fail(Status::kError, "NVENC session is closed");
    if (!packet || !frame.y || !frame.uv || frame.width != config_.width ||
        frame.height != config_.height || frame.stride_y < frame.width ||
        frame.stride_uv < frame.width) {
      return Fail(Status::kInvalidArgument, "Invalid NV12 frame or output");
    }
    Buffer& buffer = buffers_[next_buffer_];
    const uint64_t deadline = GetTickCount64() + kStagingMapTimeoutMs;
    D3D11_MAPPED_SUBRESOURCE mapping = {};
    HRESULT hr;
    do {
      hr = context_->Map(buffer.staging.Get(), 0, D3D11_MAP_WRITE,
                         D3D11_MAP_FLAG_DO_NOT_WAIT, &mapping);
      if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
        break;
      if (GetTickCount64() >= deadline)
        return Poison(Status::kTimeout, "NV12 staging map timed out");
      Sleep(1);
    } while (true);
    if (FAILED(hr))
      return Poison(Status::kError, "D3D11 Map", hr);
    if (!mapping.pData || mapping.RowPitch < static_cast<UINT>(frame.width)) {
      context_->Unmap(buffer.staging.Get(), 0);
      return Poison(Status::kError, "Invalid NV12 staging pitch");
    }
    auto* destination = static_cast<uint8_t*>(mapping.pData);
    for (int row = 0; row < frame.height; ++row) {
      std::memcpy(destination + static_cast<size_t>(row) * mapping.RowPitch,
                  frame.y + static_cast<size_t>(row) * frame.stride_y,
                  frame.width);
    }
    destination += static_cast<size_t>(mapping.RowPitch) * frame.height;
    for (int row = 0; row < frame.height / 2; ++row) {
      std::memcpy(destination + static_cast<size_t>(row) * mapping.RowPitch,
                  frame.uv + static_cast<size_t>(row) * frame.stride_uv,
                  frame.width);
    }
    context_->Unmap(buffer.staging.Get(), 0);
    context_->CopyResource(buffer.texture.Get(), buffer.staging.Get());
    context_->Flush();

    // NvEncMapInputResource synchronizes previously submitted D3D11 work.
    NV_ENC_MAP_INPUT_RESOURCE input = {};
    input.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    input.registeredResource = buffer.registered;
    NVENCSTATUS status = api_.nvEncMapInputResource(session_, &input);
    if (status != NV_ENC_SUCCESS)
      return Poison(Status::kError, "NvEncMapInputResource", status);
    buffer.mapped = input.mappedResource;
    if (input.mappedBufferFmt != NV_ENC_BUFFER_FORMAT_NV12)
      return Poison(Status::kError, "Mapped input is not NV12");

    NV_ENC_PIC_PARAMS picture = {};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = frame.width;
    picture.inputHeight = frame.height;
    picture.inputPitch = frame.width;
    picture.inputBuffer = buffer.mapped;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picture.outputBitstream = buffer.bitstream;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = frame.id;
    picture.frameIdx = frame_index_++;
    if (force_key_frame || !encode_started_) {
      picture.encodePicFlags =
          NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    encode_started_ = true;
    // Treat errors conservatively: Close must retire any possible submission.
    buffer.submitted = true;
    if (picture.frameIdx == 0) {
      RTC_LOG(LS_INFO) << "NVENC first EncodePicture begin frame_id="
                       << frame.id << " slot=" << next_buffer_;
    }
    status = api_.nvEncEncodePicture(session_, &picture);
    if (status != NV_ENC_SUCCESS) {
      return Poison(Status::kError, "NvEncEncodePicture", status);
    }

    NV_ENC_LOCK_BITSTREAM output = {};
    output.version = NV_ENC_LOCK_BITSTREAM_VER;
    output.outputBitstream = buffer.bitstream;
    // Use the synchronous blocking output path used by OBS and FFmpeg.
    // This driver call has no hard deadline.
    output.doNotWait = 0;
    const uint64_t lock_started = GetTickCount64();
    if (picture.frameIdx == 0) {
      RTC_LOG(LS_INFO)
          << "NVENC first EncodePicture succeeded; LockBitstream begin"
          << " async=" << initialize_.enableEncodeAsync
          << " do_not_wait=" << output.doNotWait
          << " version=" << Hex(output.version) << " frame_id=" << frame.id
          << " slot=" << next_buffer_;
    }
    status = api_.nvEncLockBitstream(session_, &output);
    if (status != NV_ENC_SUCCESS)
      return Poison(Status::kError, "NvEncLockBitstream", status);
    buffer.locked = true;
    if (picture.frameIdx == 0) {
      RTC_LOG(LS_INFO) << "NVENC first LockBitstream succeeded elapsed_ms="
                       << GetTickCount64() - lock_started
                       << " raw_hw_status=" << Hex(output.hwEncodeStatus)
                       << " bytes=" << output.bitstreamSizeInBytes
                       << " frame_id=" << output.outputTimeStamp
                       << " data_present="
                       << (output.bitstreamBufferPtr != nullptr)
                       << " picture_type="
                       << static_cast<int>(output.pictureType);
    }
    buffer.submitted = false;
    const uint64_t maximum_size =
        static_cast<uint64_t>(frame.width) * frame.height * 4 + 65536;
    // The SDK does not define success values for the raw hwEncodeStatus field.
    // Like OBS/FFmpeg, use the API return status, then validate the output
    // data.
    bool valid_output = output.outputTimeStamp == frame.id &&
                        output.bitstreamBufferPtr &&
                        output.bitstreamSizeInBytes > 0 &&
                        output.bitstreamSizeInBytes <= maximum_size;
    if (!valid_output) {
      RTC_LOG(LS_ERROR) << "NVENC invalid output expected_frame_id=" << frame.id
                        << " actual_frame_id=" << output.outputTimeStamp
                        << " data_present="
                        << (output.bitstreamBufferPtr != nullptr)
                        << " bytes=" << output.bitstreamSizeInBytes
                        << " maximum_bytes=" << maximum_size
                        << " raw_hw_status=" << Hex(output.hwEncodeStatus);
    }
    if (valid_output) {
      const auto* bytes =
          static_cast<const uint8_t*>(output.bitstreamBufferPtr);
      packet->bytes.assign(bytes, bytes + output.bitstreamSizeInBytes);
      packet->frame_id = output.outputTimeStamp;
      packet->key_frame = output.pictureType == NV_ENC_PIC_TYPE_IDR;
      packet->qp =
          output.frameAvgQP <= 51 ? static_cast<int>(output.frameAvgQP) : -1;
    }
    status = api_.nvEncUnlockBitstream(session_, buffer.bitstream);
    if (status != NV_ENC_SUCCESS) {
      *packet = Packet{};
      return Poison(Status::kError, "NvEncUnlockBitstream", status);
    }
    buffer.locked = false;
    status = api_.nvEncUnmapInputResource(session_, buffer.mapped);
    if (status != NV_ENC_SUCCESS) {
      *packet = Packet{};
      return Poison(Status::kError, "NvEncUnmapInputResource", status);
    }
    buffer.mapped = nullptr;
    if (!valid_output)
      return Poison(Status::kError, "Invalid or mismatched NVENC output");
    next_buffer_ = (next_buffer_ + 1) % kBufferCount;
    return Status::kOk;
  }

  void Close() override {
    CloseSession();
    if (retirement_unconfirmed_) {
      return;
    }
    if (library_) {
      FreeLibrary(library_);
      library_ = nullptr;
    }
    api_ = {};
  }

  Status LastCloseStatus() const override { return last_close_status_; }
  std::string_view LastError() const override { return last_error_; }

 private:
  const GUID& CodecGuid() const {
    return codec_ == Codec::kH264 ? NV_ENC_CODEC_H264_GUID
                                  : NV_ENC_CODEC_HEVC_GUID;
  }

  const GUID& ProfileGuid() const {
    return codec_ == Codec::kH264 ? NV_ENC_H264_PROFILE_BASELINE_GUID
                                  : NV_ENC_HEVC_PROFILE_MAIN_GUID;
  }

  const char* CodecName() const {
    return codec_ == Codec::kH264 ? "H.264" : "HEVC";
  }

  Status Fail(Status status, const char* message) {
    if (last_close_status_ != Status::kOk) {
      return status;
    }
    last_error_ = message;
    return status;
  }

  Status Fail(Status status, const char* operation, uint32_t error) {
    if (last_close_status_ != Status::kOk) {
      return status;
    }
    char code[32];
    std::snprintf(code, sizeof(code), " failed (0x%08lx)",
                  static_cast<unsigned long>(error));
    last_error_ = std::string(operation) + code;
    return status;
  }

  Status Fail(Status status, const char* operation, NVENCSTATUS error) {
    if (last_close_status_ != Status::kOk) {
      return status;
    }
    Fail(status, operation, static_cast<uint32_t>(error));
    last_error_ += std::string(" ") + NvencErrorName(error);
    // Copy the driver's error before cleanup or any other NVENC call can
    // replace it. This optional entry is not a hardware-availability gate.
    if (session_ && api_.nvEncGetLastErrorString) {
      const char* details = api_.nvEncGetLastErrorString(session_);
      if (details && *details)
        last_error_ += std::string("; driver: ") + details;
    }
    return status;
  }

  Status Poison(Status status, const char* message) {
    poisoned_ = true;
    return Fail(status, message);
  }

  Status Poison(Status status, const char* operation, uint32_t error) {
    poisoned_ = true;
    return Fail(status, operation, error);
  }

  Status Poison(Status status, const char* operation, NVENCSTATUS error) {
    poisoned_ = true;
    return Fail(status, operation, error);
  }

  Status LoadApi() {
    // Driver DLLs come from System32, never the working directory or SDK path.
    library_ = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr,
                              LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library_)
      return Fail(Status::kUnavailable, "LoadLibraryExW(nvEncodeAPI64)",
                  GetLastError());
    using GetVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto get_version = reinterpret_cast<GetVersion>(
        GetProcAddress(library_, "NvEncodeAPIGetMaxSupportedVersion"));
    auto create_instance = reinterpret_cast<CreateInstance>(
        GetProcAddress(library_, "NvEncodeAPICreateInstance"));
    if (!get_version || !create_instance)
      return Fail(Status::kUnavailable, "Missing NVENC driver entry point");
    uint32_t driver_version = 0;
    NVENCSTATUS status = get_version(&driver_version);
    if (status != NV_ENC_SUCCESS)
      return Fail(Status::kUnavailable, "NvEncodeAPIGetMaxSupportedVersion",
                  status);
    constexpr uint32_t required_version =
        (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    RTC_LOG(LS_INFO) << "NVENC driver_api=" << (driver_version >> 4) << "."
                     << (driver_version & 0xf)
                     << " required_api=" << NVENCAPI_MAJOR_VERSION << "."
                     << NVENCAPI_MINOR_VERSION
                     << " driver_api_raw=" << Hex(driver_version)
                     << " required_api_raw=" << Hex(required_version);
    if (driver_version < required_version)
      return Fail(Status::kUnsupported, "NVENC driver API is older than SDK");
    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    status = create_instance(&api_);
    if (status != NV_ENC_SUCCESS)
      return Fail(Status::kUnavailable, "NvEncodeAPICreateInstance", status);
    if (!api_.nvEncOpenEncodeSessionEx || !api_.nvEncGetEncodeGUIDCount ||
        !api_.nvEncGetEncodeGUIDs || !api_.nvEncGetEncodeProfileGUIDCount ||
        !api_.nvEncGetEncodeProfileGUIDs || !api_.nvEncGetInputFormatCount ||
        !api_.nvEncGetInputFormats || !api_.nvEncGetEncodePresetCount ||
        !api_.nvEncGetEncodePresetGUIDs || !api_.nvEncGetEncodeCaps ||
        !api_.nvEncGetEncodePresetConfigEx || !api_.nvEncInitializeEncoder ||
        !api_.nvEncRegisterResource || !api_.nvEncUnregisterResource ||
        !api_.nvEncMapInputResource || !api_.nvEncUnmapInputResource ||
        !api_.nvEncCreateBitstreamBuffer || !api_.nvEncDestroyBitstreamBuffer ||
        !api_.nvEncEncodePicture || !api_.nvEncLockBitstream ||
        !api_.nvEncUnlockBitstream || !api_.nvEncReconfigureEncoder ||
        !api_.nvEncDestroyEncoder) {
      return Fail(Status::kUnavailable, "Incomplete NVENC function table");
    }
    return Status::kOk;
  }

  Status GetCapability(NV_ENC_CAPS capability, int* value) {
    NV_ENC_CAPS_PARAM parameter = {};
    parameter.version = NV_ENC_CAPS_PARAM_VER;
    parameter.capsToQuery = capability;
    NVENCSTATUS status =
        api_.nvEncGetEncodeCaps(session_, CodecGuid(), &parameter, value);
    if (status != NV_ENC_SUCCESS)
      return Fail(Status::kUnsupported, "NvEncGetEncodeCaps", status);
    return Status::kOk;
  }

  Status QueryFailure(const char* kind,
                      const internal::CapabilityQueryResult& result) {
    last_error_ =
        std::string("NVENC ") + kind + " enumeration " +
        QueryErrorName(result.error) +
        " (queried_count=" + std::to_string(result.queried_count) +
        ", filled_count=" + std::to_string(result.filled_count) +
        ", count_status=" + Hex(result.count_status) + ", list_status=" +
        (result.list_status ? Hex(*result.list_status) : "not_called") + ")";
    return result.error == internal::CapabilityQueryError::kEmptyCount
               ? Status::kUnavailable
               : Status::kError;
  }

  Status ConfigureSession() {
    RTC_LOG(LS_INFO) << "NVENC requested codec=" << CodecName()
                     << " codec_guid=" << FormatGuid(CodecGuid())
                     << " profile_guid=" << FormatGuid(ProfileGuid());
    std::vector<GUID> guids;
    auto result = internal::QueryNvencCapabilities<GUID>(
        [this](uint32_t* count) {
          return api_.nvEncGetEncodeGUIDCount(session_, count);
        },
        [this](GUID* values, uint32_t capacity, uint32_t* filled) {
          return api_.nvEncGetEncodeGUIDs(session_, values, capacity, filled);
        },
        &guids);
    LogQuery("codec", result);
    if (!result.ok())
      return QueryFailure("codec", result);
    LogGuids("codec", guids);
    if (AllZeroGuids(guids))
      return Fail(Status::kError, "Invalid zero codec GUID list");
    if (std::none_of(guids.begin(), guids.end(), [this](const GUID& guid) {
          return EqualGuid(guid, CodecGuid());
        })) {
      return Fail(
          Status::kUnsupported,
          codec_ == Codec::kH264
              ? "H.264 GUID is absent from the returned NVENC codec list"
              : "HEVC GUID is absent from the returned NVENC codec list");
    }
    result = internal::QueryNvencCapabilities<GUID>(
        [this](uint32_t* count) {
          return api_.nvEncGetEncodeProfileGUIDCount(session_, CodecGuid(),
                                                     count);
        },
        [this](GUID* values, uint32_t capacity, uint32_t* filled) {
          return api_.nvEncGetEncodeProfileGUIDs(session_, CodecGuid(), values,
                                                 capacity, filled);
        },
        &guids);
    LogQuery("profile", result);
    if (!result.ok())
      return QueryFailure("profile", result);
    LogGuids("profile", guids);
    if (AllZeroGuids(guids))
      return Fail(Status::kError, "Invalid zero profile GUID list");
    if (std::none_of(guids.begin(), guids.end(), [this](const GUID& guid) {
          return EqualGuid(guid, ProfileGuid());
        })) {
      return Fail(
          Status::kUnsupported,
          codec_ == Codec::kH264
              ? "Baseline GUID is absent from the returned NVENC profile list"
              : "Main GUID is absent from the returned NVENC profile list");
    }
    result = internal::QueryNvencCapabilities<GUID>(
        [this](uint32_t* count) {
          return api_.nvEncGetEncodePresetCount(session_, CodecGuid(), count);
        },
        [this](GUID* values, uint32_t capacity, uint32_t* filled) {
          return api_.nvEncGetEncodePresetGUIDs(session_, CodecGuid(), values,
                                                capacity, filled);
        },
        &guids);
    LogQuery("preset", result);
    if (!result.ok())
      return QueryFailure("preset", result);
    LogGuids("preset", guids);
    if (AllZeroGuids(guids))
      return Fail(Status::kError, "Invalid zero preset GUID list");
    if (std::none_of(guids.begin(), guids.end(), [](const GUID& guid) {
          return EqualGuid(guid, NV_ENC_PRESET_P4_GUID);
        })) {
      return Fail(Status::kUnsupported,
                  "P4 GUID is absent from the returned NVENC preset list");
    }
    std::vector<NV_ENC_BUFFER_FORMAT> formats;
    result = internal::QueryNvencCapabilities<NV_ENC_BUFFER_FORMAT>(
        [this](uint32_t* count) {
          return api_.nvEncGetInputFormatCount(session_, CodecGuid(), count);
        },
        [this](NV_ENC_BUFFER_FORMAT* values, uint32_t capacity,
               uint32_t* filled) {
          return api_.nvEncGetInputFormats(session_, CodecGuid(), values,
                                           capacity, filled);
        },
        &formats);
    LogQuery("input_format", result);
    if (!result.ok())
      return QueryFailure("input_format", result);
    for (size_t index = 0; index < formats.size(); ++index) {
      RTC_LOG(LS_INFO) << "NVENC input_format[" << index
                       << "]=" << Hex(static_cast<uint32_t>(formats[index]));
    }
    if (std::find(formats.begin(), formats.end(), NV_ENC_BUFFER_FORMAT_NV12) ==
        formats.end()) {
      return Fail(Status::kUnsupported,
                  "NV12 is absent from the returned NVENC input format list");
    }
    int max_width = 0, max_height = 0, dynamic_bitrate = 0, rate_modes = 0;
    for (auto pair :
         {std::pair{NV_ENC_CAPS_WIDTH_MAX, &max_width},
          std::pair{NV_ENC_CAPS_HEIGHT_MAX, &max_height},
          std::pair{NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE, &dynamic_bitrate},
          std::pair{NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES, &rate_modes}}) {
      Status capability_status = GetCapability(pair.first, pair.second);
      if (capability_status != Status::kOk)
        return capability_status;
    }
    if (config_.width > max_width || config_.height > max_height ||
        !dynamic_bitrate || !(rate_modes & NV_ENC_PARAMS_RC_CBR)) {
      return Fail(Status::kUnsupported,
                  "Adapter lacks requested size, dynamic bitrate, or CBR");
    }
    if (codec_ == Codec::kH265) {
      int min_level = 0;
      int max_level = 0;
      Status capability_status =
          GetCapability(NV_ENC_CAPS_LEVEL_MIN, &min_level);
      if (capability_status != Status::kOk)
        return capability_status;
      capability_status = GetCapability(NV_ENC_CAPS_LEVEL_MAX, &max_level);
      if (capability_status != Status::kOk)
        return capability_status;
      RTC_LOG(LS_INFO) << "NVENC HEVC level min=" << min_level
                       << " max=" << max_level
                       << " requested=" << static_cast<int>(config_.h265_level);
      if (config_.h265_level < min_level || config_.h265_level > max_level) {
        return Fail(Status::kUnsupported,
                    "Requested HEVC level is outside adapter capabilities");
      }
    }

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    NVENCSTATUS status = api_.nvEncGetEncodePresetConfigEx(
        session_, CodecGuid(), NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (status != NV_ENC_SUCCESS)
      return Fail(Status::kUnsupported, "NvEncGetEncodePresetConfigEx", status);
    encode_config_ = preset.presetCfg;
    encode_config_.profileGUID = ProfileGuid();
    encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
    encode_config_.frameIntervalP = 1;
    encode_config_.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encode_config_.rcParams.enableLookahead = 0;
    encode_config_.rcParams.enableExtLookahead = 0;
    encode_config_.rcParams.lookaheadDepth = 0;
    encode_config_.rcParams.enableAQ = 0;
    encode_config_.rcParams.enableTemporalAQ = 0;
    encode_config_.rcParams.zeroReorderDelay = 1;
    encode_config_.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    ApplyRates({config_.bitrate_bps, config_.framerate_fps}, &encode_config_);
    if (codec_ == Codec::kH264) {
      auto& h264 = encode_config_.encodeCodecConfig.h264Config;
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
    } else {
      auto& hevc = encode_config_.encodeCodecConfig.hevcConfig;
      hevc.level = config_.h265_level;
      hevc.tier = NV_ENC_TIER_HEVC_MAIN;
      hevc.enableIntraRefresh = 0;
      hevc.enableLTR = 0;
      hevc.enableAlphaLayerEncoding = 0;
      hevc.maxTemporalLayersMinus1 = 0;
      hevc.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
      hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
      // In HEVC, this flag repeats all three parameter sets: VPS/SPS/PPS.
      hevc.repeatSPSPPS = 1;
      hevc.disableSPSPPS = 0;
      hevc.outputAUD = 0;
      hevc.enableFillerDataInsertion = 0;
      hevc.chromaFormatIDC = 1;
      hevc.maxNumRefFramesInDPB = 1;
      hevc.sliceMode = 3;
      hevc.sliceModeData = 1;
      hevc.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
      hevc.inputBitDepth = NV_ENC_BIT_DEPTH_8;
      hevc.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    }
    initialize_ = {};
    initialize_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize_.encodeGUID = CodecGuid();
    initialize_.presetGUID = NV_ENC_PRESET_P4_GUID;
    initialize_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initialize_.encodeWidth = config_.width;
    initialize_.encodeHeight = config_.height;
    initialize_.darWidth = config_.width;
    initialize_.darHeight = config_.height;
    initialize_.frameRateNum = config_.framerate_fps;
    initialize_.frameRateDen = 1;
    initialize_.enablePTD = 1;
    initialize_.enableEncodeAsync = 0;
    initialize_.encodeConfig = &encode_config_;
    status = api_.nvEncInitializeEncoder(session_, &initialize_);
    if (status != NV_ENC_SUCCESS)
      return Fail(Status::kUnsupported, "NvEncInitializeEncoder", status);
    initialized_ = true;
    for (Buffer& buffer : buffers_) {
      Status buffer_status = AllocateBuffer(&buffer);
      if (buffer_status != Status::kOk)
        return buffer_status;
    }
    RTC_LOG(LS_INFO) << "NVENC initialized codec=" << CodecName()
                     << " width=" << config_.width
                     << " height=" << config_.height
                     << " bitrate_bps=" << config_.bitrate_bps
                     << " fps=" << config_.framerate_fps
                     << (codec_ == Codec::kH264 ? " h264_level="
                                                : " h265_level=")
                     << static_cast<int>(codec_ == Codec::kH264
                                             ? config_.h264_level
                                             : config_.h265_level)
                     << " async=" << initialize_.enableEncodeAsync
                     << " buffers=" << kBufferCount;
    return Status::kOk;
  }

  Status AllocateBuffer(Buffer* buffer) {
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = config_.width;
    description.Height = config_.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&description, nullptr,
                                          buffer->texture.GetAddressOf());
    if (FAILED(hr))
      return Fail(Status::kError, "CreateTexture2D(NV12)", hr);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateTexture2D(&description, nullptr,
                                  buffer->staging.GetAddressOf());
    if (FAILED(hr))
      return Fail(Status::kError, "CreateTexture2D(staging)", hr);
    NV_ENC_REGISTER_RESOURCE resource = {};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.resourceToRegister = buffer->texture.Get();
    resource.width = config_.width;
    resource.height = config_.height;
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    NVENCSTATUS status = api_.nvEncRegisterResource(session_, &resource);
    if (status != NV_ENC_SUCCESS)
      return Fail(Status::kError, "NvEncRegisterResource", status);
    buffer->registered = resource.registeredResource;
    NV_ENC_CREATE_BITSTREAM_BUFFER output = {};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    status = api_.nvEncCreateBitstreamBuffer(session_, &output);
    if (status != NV_ENC_SUCCESS)
      return Fail(Status::kError, "NvEncCreateBitstreamBuffer", status);
    buffer->bitstream = output.bitstreamBuffer;
    return Status::kOk;
  }

  void RecordCloseResult(const char* operation, NVENCSTATUS status) {
    if (status == NV_ENC_SUCCESS) {
      return;
    }
    // A later no-op Close must not erase failed SDK retirement. Reusing this
    // backend after failure would hide the unresolved session ownership.
    last_close_status_ = Status::kError;
    const std::string diagnostic = std::string(operation) + " failed (" +
                                   Hex(static_cast<uint32_t>(status)) + ") " +
                                   NvencErrorName(status);
    if (!last_error_.empty()) {
      last_error_ += "; ";
    }
    last_error_ += "close: " + diagnostic;
    HardwareEncoderProbes().RecordRetirementFailure(
        HardwareEncoderVendor::kNvenc, last_error_);
    RTC_LOG(LS_WARNING) << "NVENC " << diagnostic;
  }

  void CloseSession() {
    if (retirement_unconfirmed_) {
      return;
    }
    bool flushed = true;
    if (session_ && initialized_ && encode_started_) {
      NV_ENC_PIC_PARAMS eos = {};
      eos.version = NV_ENC_PIC_PARAMS_VER;
      eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
      // Synchronous EOS retires submissions before unregistering resources.
      // The SDK call itself has no cancellation/deadline contract.
      NVENCSTATUS status = api_.nvEncEncodePicture(session_, &eos);
      flushed = status == NV_ENC_SUCCESS;
      RecordCloseResult("NvEncEncodePicture(EOS)", status);
    }
    if (session_ && !flushed) {
      // DestroyEncoder requires a completed flush and freed input/output
      // resources. A failed EOS supplies no documented abort alternative.
      PreserveUnretiredSession("EOS failed; DestroyEncoder not attempted");
      return;
    }
    for (Buffer& buffer : buffers_) {
      if (session_) {
        if (buffer.mapped) {
          if (buffer.submitted && buffer.bitstream && !buffer.locked) {
            NV_ENC_LOCK_BITSTREAM output = {};
            output.version = NV_ENC_LOCK_BITSTREAM_VER;
            output.outputBitstream = buffer.bitstream;
            const NVENCSTATUS status =
                api_.nvEncLockBitstream(session_, &output);
            RecordCloseResult("NvEncLockBitstream(close)", status);
            if (status != NV_ENC_SUCCESS) {
              // Do not unregister a resource whose submission is unretired.
              continue;
            }
            buffer.locked = true;
            buffer.submitted = false;
          }
          if (buffer.locked) {
            const NVENCSTATUS status =
                api_.nvEncUnlockBitstream(session_, buffer.bitstream);
            RecordCloseResult("NvEncUnlockBitstream(close)", status);
            if (status != NV_ENC_SUCCESS) {
              continue;
            }
            buffer.locked = false;
          }
          const NVENCSTATUS status =
              api_.nvEncUnmapInputResource(session_, buffer.mapped);
          RecordCloseResult("NvEncUnmapInputResource(close)", status);
          if (status != NV_ENC_SUCCESS) {
            continue;
          }
          buffer.mapped = nullptr;
        }
        if (buffer.registered) {
          const NVENCSTATUS status =
              api_.nvEncUnregisterResource(session_, buffer.registered);
          RecordCloseResult("NvEncUnregisterResource(close)", status);
          if (status != NV_ENC_SUCCESS) {
            continue;
          }
          buffer.registered = nullptr;
        }
        if (buffer.bitstream) {
          const NVENCSTATUS status =
              api_.nvEncDestroyBitstreamBuffer(session_, buffer.bitstream);
          RecordCloseResult("NvEncDestroyBitstreamBuffer(close)", status);
          if (status == NV_ENC_SUCCESS) {
            buffer.bitstream = nullptr;
          }
        }
      }
    }
    if (session_) {
      for (const Buffer& buffer : buffers_) {
        if (buffer.mapped || buffer.registered || buffer.bitstream ||
            buffer.locked || buffer.submitted) {
          // The SDK requires all input/output resources to be freed before
          // DestroyEncoder. Do not assume it can abort an incomplete cleanup.
          PreserveUnretiredSession(
              "resources remain; DestroyEncoder not attempted");
          return;
        }
      }
      const NVENCSTATUS destroyed = api_.nvEncDestroyEncoder(session_);
      RecordCloseResult("NvEncDestroyEncoder(close)", destroyed);
      if (destroyed != NV_ENC_SUCCESS) {
        PreserveUnretiredSession("DestroyEncoder failed");
        return;
      }
      session_ = nullptr;
    }
    // Release host textures only after successful destruction (or when no
    // session was created). No outstanding SDK handles may reach this point.
    for (Buffer& buffer : buffers_) {
      buffer = Buffer{};
    }
    context_.Reset();
    device_.Reset();
    initialized_ = false;
    encode_started_ = false;
    next_buffer_ = 0;
    frame_index_ = 0;
  }

  void PreserveUnretiredSession(const char* reason) {
    retirement_unconfirmed_ = true;
    poisoned_ = true;
    last_close_status_ = Status::kError;
    if (!last_error_.empty()) {
      last_error_ += "; ";
    }
    last_error_ +=
        std::string("session resources retained until process exit: ") + reason;
    HardwareEncoderProbes().RecordRetirementFailure(
        HardwareEncoderVendor::kNvenc, last_error_);
    RTC_LOG(LS_WARNING) << "NVENC " << reason
                        << "; session resources retained until process exit";
  }

  const Codec codec_;
  std::unique_ptr<NvencSessionResources> resources_ =
      std::make_unique<NvencSessionResources>();
  HMODULE& library_ = resources_->library;
  NV_ENCODE_API_FUNCTION_LIST& api_ = resources_->api;
  void*& session_ = resources_->session;
  ComPtr<ID3D11Device>& device_ = resources_->device;
  ComPtr<ID3D11DeviceContext>& context_ = resources_->context;
  std::array<Buffer, kBufferCount>& buffers_ = resources_->buffers;
  Config config_;
  NV_ENC_CONFIG& encode_config_ = resources_->encode_config;
  NV_ENC_INITIALIZE_PARAMS& initialize_ = resources_->initialize;
  std::string last_error_;
  Status last_close_status_ = Status::kOk;
  bool initialized_ = false;
  bool encode_started_ = false;
  bool poisoned_ = false;
  bool retirement_unconfirmed_ = false;
  size_t next_buffer_ = 0;
  uint32_t frame_index_ = 0;
};

}  // namespace

std::unique_ptr<VideoEncoderBackend> CreateNvencH264Backend() {
  return std::make_unique<NvencBackend>(Codec::kH264);
}

std::unique_ptr<VideoEncoderBackend> CreateNvencH265Backend() {
  return std::make_unique<NvencBackend>(Codec::kH265);
}

namespace {

Status ProbeNvencBackend(Codec codec,
                         std::string* reason,
                         uint8_t h265_level = 93) {
  NvencBackend backend(codec);
  Config config;
  config.codec = codec;
  config.width = 320;
  config.height = 240;
  config.bitrate_bps = 300000;
  config.framerate_fps = 30;
  if (codec == Codec::kH265) {
    // Keep the probe within HEVC Main level 1 limits so that low negotiated
    // levels are not rejected because of the probe's own dimensions or rates.
    config.width = 160;
    config.height = 128;
    config.bitrate_bps = 64000;
    config.framerate_fps = 15;
    config.h265_level = h265_level;
  }
  Status status = backend.Open(config);
  backend.Close();
  if (backend.LastCloseStatus() != Status::kOk) {
    status = Status::kRetirementFailed;
  }
  if (reason)
    reason->assign(backend.LastError());
  return status;
}

}  // namespace

Status ProbeNvencH264Backend(std::string* reason) {
  return HardwareEncoderProbes().Run(
      HardwareEncoderVendor::kNvenc, reason, [](std::string* diagnostic) {
        return ProbeNvencBackend(Codec::kH264, diagnostic);
      });
}

Status ProbeNvencH265Backend(std::string* reason, uint8_t level) {
  return HardwareEncoderProbes().Run(
      HardwareEncoderVendor::kNvenc, reason, [level](std::string* diagnostic) {
        return ProbeNvencBackend(Codec::kH265, diagnostic, level);
      });
}

}  // namespace webrtc::external_encoder
