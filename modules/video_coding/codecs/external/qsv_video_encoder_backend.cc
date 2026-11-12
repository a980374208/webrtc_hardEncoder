/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/qsv_video_encoder_backend.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <malloc.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include "modules/video_coding/codecs/external/qsv_device_handle.h"
#if defined(RTC_ENABLE_H265)
#include "modules/video_coding/codecs/external/qsv_hevc_dimensions.h"
#endif
#include "rtc_base/logging.h"

namespace webrtc::external_encoder {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr UINT kIntelVendor = 0x8086;
constexpr size_t kMaximumBuffer = 64 * 1024 * 1024;

struct AlignedFree {
  void operator()(uint8_t* data) const { _aligned_free(data); }
};
using AlignedBytes = std::unique_ptr<uint8_t, AlignedFree>;

// EncodeFrameAsync can retain addresses of surface/bitstream/control as well
// as their buffers. Keep them in one stable block throughout SDK ownership.
struct QsvSessionResources {
  mfxLoader loader = nullptr;
  mfxSession session = nullptr;
  ComPtr<ID3D11Device> device;
  mfxVideoParam parameters = {};
  mfxExtCodingOption options = {};
  mfxExtCodingOption2 options2 = {};
  mfxExtHEVCParam hevc = {};
  std::vector<mfxExtBuffer*> extensions;
  mfxFrameSurface1 surface = {};
  mfxBitstream bitstream = {};
  mfxEncodeCtrl control = {};
  mfxSyncPoint pending = nullptr;
  AlignedBytes pixels;
  AlignedBytes output;
};

std::string AdapterName(const DXGI_ADAPTER_DESC& desc) {
  char name[512] = {};
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name),
                      nullptr, nullptr);
  return name;
}

// Vendor output and cached parameter sets must be Annex B, not AVCC lengths.
bool IsNalType(Codec codec, const uint8_t* bytes, size_t size, uint8_t type) {
  if (codec == Codec::kH264) {
    return size > 0 && (bytes[0] & 0x1f) == type;
  }
  // One-layer HEVC requires a complete two-byte header with layer_id=0 and
  // temporal_id_plus1=1. Reject forbidden_zero_bit and truncated headers.
  return size >= 2 && (bytes[0] & 0x80) == 0 &&
         ((bytes[0] >> 1) & 0x3f) == type && (bytes[0] & 1) == 0 &&
         bytes[1] == 1;
}

bool HasNal(Codec codec, const uint8_t* bytes, size_t size, uint8_t type) {
  for (size_t i = 0; i + 3 < size; ++i) {
    if (bytes[i] != 0 || bytes[i + 1] != 0) {
      continue;
    }
    size_t prefix = bytes[i + 2] == 1 ? 3 : 0;
    if (i + 4 < size && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
      prefix = 4;
    }
    if (prefix && i + prefix < size &&
        IsNalType(codec, bytes + i + prefix, size - i - prefix, type)) {
      return true;
    }
  }
  return false;
}

bool SaveParameterSet(Codec codec,
                      const uint8_t* bytes,
                      size_t size,
                      uint8_t type,
                      std::vector<uint8_t>* output) {
  output->clear();
  if (HasNal(codec, bytes, size, type)) {
    output->assign(bytes, bytes + size);
    return true;
  }
  if (IsNalType(codec, bytes, size, type)) {
    *output = {0, 0, 0, 1};
    output->insert(output->end(), bytes, bytes + size);
    return true;
  }
  return false;
}

bool ValidRates(const Rates& rates) {
  return rates.bitrate_bps > 0 && rates.bitrate_bps <= 65535000 &&
         rates.framerate_fps > 0 && rates.framerate_fps <= 240;
}

bool ValidH265Level(uint8_t level) {
  constexpr std::array<uint8_t, 13> levels = {30,  60,  63,  90,  93,  120, 123,
                                              150, 153, 156, 180, 183, 186};
  return std::find(levels.begin(), levels.end(), level) != levels.end();
}

class QsvBackend final : public VideoEncoderBackend {
 public:
  explicit QsvBackend(Codec codec) : codec_(codec) {}
  ~QsvBackend() override {
    Close();
    if (retirement_unconfirmed_) {
      // MFXClose failure does not establish retirement. Keep this instance's
      // single bounded block at its original addresses until process exit.
      // No global quarantine or additional sessions are allocated.
      (void)resources_.release();
    }
  }

  Info GetInfo() const override {
    return {codec_ == Codec::kH264 ? "QSV H264 / oneVPL D3D11"
                                   : "QSV H265 / oneVPL D3D11",
            true};
  }
  Status LastCloseStatus() const override { return last_close_status_; }
  std::string_view LastError() const override { return error_; }
  const std::string& Diagnostic() const { return diagnostic_; }

  Status Open(const Config& config) override {
    Close();
    std::string retirement_reason;
    if (HardwareEncoderProbes().Check(HardwareEncoderVendor::kQsv,
                                      &retirement_reason) ==
        Status::kRetirementFailed) {
      return Fail(Status::kRetirementFailed, retirement_reason);
    }
    if (last_close_status_ != Status::kOk) {
      return last_close_status_;
    }
    error_.clear();
    diagnostic_.clear();
    if (config.codec != codec_ || config.width <= 0 || config.height <= 0 ||
        config.width > 8192 || config.height > 8192 || config.width % 2 ||
        config.height % 2 ||
        (codec_ == Codec::kH264 ? config.h264_level == 0
                                : !ValidH265Level(config.h265_level)) ||
        !ValidRates({config.bitrate_bps, config.framerate_fps})) {
      return Fail(Status::kInvalidArgument,
                  "invalid codec/level/NV12 configuration");
    }
    config_ = config;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return Fail(Status::kUnavailable, "DXGI factory unavailable");
    }
    Status last_status = Status::kUnavailable;
    bool found_intel = false;
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
      if (enumerated == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(enumerated)) {
        return Fail(Status::kError, "DXGI adapter enumeration failed");
      }
      DXGI_ADAPTER_DESC desc = {};
      if (FAILED(adapter->GetDesc(&desc)) || desc.VendorId != kIntelVendor) {
        continue;
      }
      found_intel = true;
      last_status = OpenAdapter(adapter.Get(), index, desc);
      if (last_status == Status::kOk) {
        error_.clear();
        return Status::kOk;
      }
      Close();
      if (last_close_status_ != Status::kOk) {
        return last_close_status_;
      }
    }
    if (!found_intel) {
      return Fail(Status::kUnavailable, "no Intel DXGI graphics adapter");
    }
    return last_status;
  }

  Status SetRates(const Rates& rates) override {
    if (!initialized_) {
      return Fail(Status::kError, "QSV encoder not initialized");
    }
    if (!ValidRates(rates)) {
      return Fail(Status::kInvalidArgument, "unsupported bitrate/framerate");
    }
    if (pending_ || surface_.Data.Locked) {
      return Abort(Status::kError, "cannot reset with an unretired frame");
    }
    config_.bitrate_bps = rates.bitrate_bps;
    config_.framerate_fps = rates.framerate_fps;
    SetRateFields();
    LogConfiguration("ResetInput", MFX_ERR_NONE, parameters_, &hevc_);
    const mfxStatus reset = MFXVideoENCODE_Reset(session_, &parameters_);
    LogConfiguration("ResetResult", reset, parameters_, &hevc_);
    if (codec_ == Codec::kH265 && reset == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM) {
      // The SDK cannot apply this change within the initialization allocation.
      // Close/reinitialize as required by the Reset contract. Open owns all
      // teardown/storage, validates the new configuration and sequence headers,
      // and requests an IDR before returning to the same WebRTC adapter.
      const Config reopened_config = config_;
      RTC_LOG(LS_INFO) << "QSV_RECONFIG Reset status=" << reset
                       << " action=close_reinitialize codec=H265";
      return Open(reopened_config);
    }
    if (!AcceptConfigurationStatus(reset)) {
      return Abort(Status::kError, "MFXVideoENCODE_Reset", reset);
    }
    if (!ReadValidatedConfiguration()) {
      const std::string reason = error_;
      Close();
      if (last_close_status_ != Status::kOk) {
        return last_close_status_;
      }
      return Fail(Status::kUnsupported, reason);
    }
    if (parameters_.mfx.FrameInfo.Width != surface_.Info.Width ||
        parameters_.mfx.FrameInfo.Height != surface_.Info.Height) {
      return Abort(Status::kUnsupported,
                   "QSV reset changed allocated dimensions");
    }
    surface_.Info = parameters_.mfx.FrameInfo;
    need_idr_ = true;
    return Status::kOk;
  }

  Status Encode(const Frame& frame,
                bool force_key_frame,
                Packet* packet) override {
    if (!initialized_ || pending_ || surface_.Data.Locked) {
      return Abort(Status::kError, "QSV encoder has no idle surface");
    }
    if (!packet || !frame.y || !frame.uv || frame.width != config_.width ||
        frame.height != config_.height || frame.stride_y < frame.width ||
        frame.stride_uv < frame.width ||
        frame.id == static_cast<uint64_t>(MFX_TIMESTAMP_UNKNOWN)) {
      return Fail(Status::kInvalidArgument, "invalid input frame");
    }
    *packet = {};
    // Never lend the caller's planes to an asynchronous vendor operation.
    std::memset(pixels_.get(), 0, pixel_bytes_ * 2 / 3);
    std::memset(pixels_.get() + pixel_bytes_ * 2 / 3, 128, pixel_bytes_ / 3);
    for (int y = 0; y < frame.height; ++y) {
      std::memcpy(surface_.Data.Y + y * surface_.Data.Pitch,
                  frame.y + static_cast<size_t>(y) * frame.stride_y,
                  frame.width);
    }
    for (int y = 0; y < frame.height / 2; ++y) {
      std::memcpy(surface_.Data.UV + y * surface_.Data.Pitch,
                  frame.uv + static_cast<size_t>(y) * frame.stride_uv,
                  frame.width);
    }
    surface_.Data.TimeStamp = frame.id;
    surface_.Data.FrameOrder = static_cast<mfxU32>(frame.id);
    bitstream_.DataOffset = 0;
    bitstream_.DataLength = 0;
    bitstream_.TimeStamp = MFX_TIMESTAMP_UNKNOWN;
    bitstream_.DecodeTimeStamp = MFX_TIMESTAMP_UNKNOWN;
    bitstream_.FrameType = 0;
    bitstream_.DataFlag = 0;
    control_ = {};
    const bool request_idr = force_key_frame || need_idr_;
    if (request_idr) {
      control_.FrameType =
          MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
    }
    const auto busy_deadline = Clock::now() + std::chrono::seconds(2);
    mfxStatus submitted;
    do {
      submitted = MFXVideoENCODE_EncodeFrameAsync(
          session_, &control_, &surface_, &bitstream_, &pending_);
      if (submitted != MFX_WRN_DEVICE_BUSY || pending_) {
        break;
      }
      if (Clock::now() >= busy_deadline) {
        return Abort(Status::kTimeout, "QSV submission busy deadline",
                     submitted);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
    // MORE_DATA may already own/lock the surface. Abort this session instead of
    // dropping the frame ID or resubmitting it; retain all storage until Close.
    if (submitted != MFX_ERR_NONE || !pending_) {
      return Abort(Status::kError, "QSV expected one complete submitted frame",
                   submitted);
    }
    const auto sync_deadline = Clock::now() + std::chrono::seconds(2);
    mfxStatus synced;
    do {
      synced = MFXVideoCORE_SyncOperation(session_, pending_, 100);
      if (synced != MFX_WRN_IN_EXECUTION) {
        break;
      }
    } while (Clock::now() < sync_deadline);
    if (synced != MFX_ERR_NONE) {
      return Abort(
          synced == MFX_WRN_IN_EXECUTION ? Status::kTimeout : Status::kError,
          "MFXVideoCORE_SyncOperation", synced);
    }
    pending_ = nullptr;
    if (surface_.Data.Locked || bitstream_.TimeStamp != frame.id ||
        bitstream_.DataLength == 0 ||
        bitstream_.DataOffset > bitstream_.MaxLength ||
        bitstream_.DataLength > bitstream_.MaxLength - bitstream_.DataOffset) {
      return Abort(Status::kError,
                   "invalid QSV output ownership/timestamp/size");
    }
    const auto* bytes = bitstream_.Data + bitstream_.DataOffset;
    const size_t size = bitstream_.DataLength;
    const bool idr = codec_ == Codec::kH264
                         ? HasNal(codec_, bytes, size, 5)
                         : HasNal(codec_, bytes, size, 19) ||
                               HasNal(codec_, bytes, size, 20);
    const bool picture =
        codec_ == Codec::kH264
            ? HasNal(codec_, bytes, size, 1)
            : HasNal(codec_, bytes, size, 0) || HasNal(codec_, bytes, size, 1);
    if ((!idr && !picture) || (request_idr && !idr)) {
      return Abort(Status::kError,
                   "QSV did not emit the requested Annex B picture");
    }
    // GetVideoParam caches the active sequence headers. Prepend missing headers
    // on IDR so each requested keyframe works with a newly created decoder.
    const uint8_t sps_type = codec_ == Codec::kH264 ? 7 : 33;
    const uint8_t pps_type = codec_ == Codec::kH264 ? 8 : 34;
    if (idr && (!HasNal(codec_, bytes, size, sps_type) ||
                !HasNal(codec_, bytes, size, pps_type) ||
                (codec_ == Codec::kH265 && !HasNal(codec_, bytes, size, 32)))) {
      if (codec_ == Codec::kH265) {
        packet->bytes.insert(packet->bytes.end(), vps_.begin(), vps_.end());
      }
      packet->bytes.insert(packet->bytes.end(), sps_.begin(), sps_.end());
      packet->bytes.insert(packet->bytes.end(), pps_.begin(), pps_.end());
    }
    packet->bytes.insert(packet->bytes.end(), bytes, bytes + size);
    packet->frame_id = frame.id;
    packet->key_frame = idr;
    need_idr_ = false;
    return Status::kOk;
  }

  void Close() override {
    if (retirement_unconfirmed_) {
      return;
    }
    // Sync is best effort here. A successful MFXClose aborts active/queued
    // tasks. Host storage outlives both close calls, but failed MFXClose cannot
    // prove retirement. Preserve dependencies at their original addresses and
    // prohibit reinitialization or further SDK calls on that session.
    if (session_) {
      if (pending_) {
        const mfxStatus synced =
            MFXVideoCORE_SyncOperation(session_, pending_, 100);
        if (synced != MFX_ERR_NONE) {
          // An in-progress sync is permitted if subsequent session abort
          // succeeds. It is not independently a failed Close.
          RTC_LOG(LS_INFO) << "QSV best-effort close SyncOperation status="
                           << synced;
        }
      }
      const mfxStatus encoder_closed = MFXVideoENCODE_Close(session_);
      // Capability probing may create a session without initializing ENCODE.
      // NOT_INITIALIZED is expected only when Init never succeeded.
      if (encoder_closed != MFX_ERR_NOT_INITIALIZED || encoder_initialized_) {
        RecordCloseResult("MFXVideoENCODE_Close", encoder_closed);
      }
      const mfxStatus closed = MFXClose(session_);
      RecordCloseResult("MFXClose", closed);
      if (closed != MFX_ERR_NONE) {
        retirement_unconfirmed_ = true;
        initialized_ = false;
        return;
      }
      session_ = nullptr;
    }
    if (loader_) {
      MFXUnload(loader_);
      loader_ = nullptr;
    }
    pending_ = nullptr;
    initialized_ = false;
    encoder_initialized_ = false;
    pixels_.reset();
    output_.reset();
    device_.Reset();
    surface_ = {};
    bitstream_ = {};
    control_ = {};
    pixel_bytes_ = 0;
    vps_.clear();
    sps_.clear();
    pps_.clear();
  }

 private:
  void RecordCloseResult(const char* operation, mfxStatus status) {
    if (status == MFX_ERR_NONE) {
      return;
    }
    last_close_status_ = Status::kError;
    const std::string diagnostic =
        std::string(operation) + " status=" + std::to_string(status);
    if (!error_.empty()) {
      error_ += "; ";
    }
    error_ += "close: " + diagnostic;
    HardwareEncoderProbes().RecordRetirementFailure(HardwareEncoderVendor::kQsv,
                                                    error_);
    RTC_LOG(LS_WARNING) << "QSV " << diagnostic;
  }

  Status Fail(Status status,
              const std::string& message,
              mfxStatus vendor_status = MFX_ERR_NONE) {
    if (last_close_status_ != Status::kOk) {
      return status;
    }
    error_ = message;
    if (vendor_status != MFX_ERR_NONE) {
      error_ += " status=" + std::to_string(vendor_status);
    }
    return status;
  }
  Status Abort(Status status,
               const std::string& message,
               mfxStatus vendor_status = MFX_ERR_NONE) {
    Fail(status, message, vendor_status);
    Close();
    return status;
  }
  bool Filter(const char* name, mfxU32 value) {
    mfxVariant variant = {};
    variant.Version.Version = MFX_VARIANT_VERSION;
    variant.Type = MFX_VARIANT_TYPE_U32;
    variant.Data.U32 = value;
    return ApplyFilter(name, variant);
  }
  bool ApplyFilter(const char* name, mfxVariant value) {
    const mfxConfig filter = MFXCreateConfig(loader_);
    if (!filter) {
      Fail(Status::kError, "MFXCreateConfig");
      return false;
    }
    const auto result = MFXSetConfigFilterProperty(
        filter, reinterpret_cast<const mfxU8*>(name), value);
    if (result != MFX_ERR_NONE) {
      Fail(Status::kUnsupported, name, result);
      return false;
    }
    return true;
  }
  Status OpenAdapter(IDXGIAdapter1* adapter,
                     UINT index,
                     const DXGI_ADAPTER_DESC& desc) {
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &device_,
                                 nullptr, nullptr))) {
      return Fail(Status::kUnavailable, "Intel D3D11 device unavailable");
    }
    ComPtr<ID3D10Multithread> multithread;
    if (FAILED(device_.As(&multithread))) {
      return Fail(Status::kUnsupported, "Intel D3D11 multithread interface");
    }
    multithread->SetMultithreadProtected(TRUE);
    loader_ = MFXLoad();
    if (!loader_) {
      return Fail(Status::kUnavailable, "oneVPL loader unavailable");
    }
    if (!Filter("mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE) ||
        !Filter("mfxImplDescription.VendorID", kIntelVendor) ||
        !Filter("mfxImplDescription.AccelerationMode",
                MFX_ACCEL_MODE_VIA_D3D11) ||
        !Filter("DXGIAdapterIndex", index) ||
        !Filter("mfxHandleType", MFX_HANDLE_D3D11_DEVICE)) {
      return Status::kUnsupported;
    }
    mfxVariant handle = {};
    handle.Version.Version = MFX_VARIANT_VERSION;
    handle.Type = MFX_VARIANT_TYPE_PTR;
    handle.Data.Ptr = device_.Get();
    if (!ApplyFilter("mfxHDL", handle)) {
      return Status::kUnsupported;
    }
    // Do not filter ApiVersion >= 2 or modern full encoder capabilities:
    // the dispatcher also supplies descriptions for legacy Media SDK runtimes.
    mfxHDL description_handle = nullptr;
    auto result = MFXEnumImplementations(
        loader_, 0, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &description_handle);
    if (result != MFX_ERR_NONE || !description_handle) {
      return Fail(Status::kUnavailable, "Intel hardware runtime unavailable",
                  result);
    }
    const auto* description =
        static_cast<const mfxImplDescription*>(description_handle);
    const bool hardware = description->Impl == MFX_IMPL_TYPE_HARDWARE &&
                          description->VendorID == kIntelVendor;
    MFXDispReleaseImplDescription(loader_, description_handle);
    if (!hardware) {
      return Fail(Status::kUnsupported, "oneVPL did not select Intel hardware");
    }
    result = MFXCreateSession(loader_, 0, &session_);
    if (result != MFX_ERR_NONE || !session_) {
      return Fail(Status::kUnavailable, "MFXCreateSession", result);
    }
    mfxVersion version = {};
    mfxIMPL implementation = 0;
    if (MFXQueryVersion(session_, &version) != MFX_ERR_NONE ||
        MFXQueryIMPL(session_, &implementation) != MFX_ERR_NONE ||
        MFX_IMPL_BASETYPE(implementation) == MFX_IMPL_SOFTWARE ||
        version.Major < 1 || (version.Major == 1 && version.Minor < 6)) {
      return Fail(Status::kUnsupported, "unsupported Intel runtime/API");
    }
    mfxHDL actual_handle = nullptr;
    result = MFXVideoCORE_GetHandle(session_, MFX_HANDLE_D3D11_DEVICE,
                                    &actual_handle);
    if (result != MFX_ERR_NONE || !actual_handle) {
      return Fail(Status::kUnsupported, "QSV D3D11 handle", result);
    }
    auto actual_device = RetainQsvDeviceHandle(
        static_cast<ID3D11Device*>(actual_handle), version.Major);
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> actual_adapter;
    DXGI_ADAPTER_DESC actual_desc = {};
    if (FAILED(actual_device.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(&actual_adapter)) ||
        FAILED(actual_adapter->GetDesc(&actual_desc)) ||
        actual_desc.VendorId != kIntelVendor ||
        actual_desc.AdapterLuid.HighPart != desc.AdapterLuid.HighPart ||
        actual_desc.AdapterLuid.LowPart != desc.AdapterLuid.LowPart) {
      return Fail(Status::kUnsupported,
                  "QSV selected a different D3D11 adapter");
    }
    std::ostringstream diagnostic;
    diagnostic << "runtime API=" << version.Major << '.' << version.Minor
               << " adapter=" << AdapterName(actual_desc)
               << " LUID=" << std::hex << actual_desc.AdapterLuid.HighPart
               << ':' << actual_desc.AdapterLuid.LowPart;
    mfxHDL path = nullptr;
    if (MFXEnumImplementations(loader_, 0, MFX_IMPLCAPS_IMPLPATH, &path) ==
            MFX_ERR_NONE &&
        path) {
      diagnostic << " runtime=" << static_cast<const char*>(path);
      MFXDispReleaseImplDescription(loader_, path);
    }
    diagnostic_ = diagnostic.str();
    MakeParameters();
    mfxVideoParam requested = parameters_;
    result = MFXVideoENCODE_Query(session_, &requested, &parameters_);
    LogConfiguration("Query", result, parameters_, &hevc_);
    if (!AcceptConfigurationStatus(result)) {
      return Fail(Status::kUnsupported, "MFXVideoENCODE_Query", result);
    }
    result = MFXVideoENCODE_Init(session_, &parameters_);
    LogConfiguration("Init", result, parameters_, &hevc_);
    // A rejected positive warning can still leave an initialized component.
    encoder_initialized_ = result >= MFX_ERR_NONE;
    if (!AcceptConfigurationStatus(result)) {
      return Fail(Status::kUnsupported, "MFXVideoENCODE_Init", result);
    }
    if (!ReadValidatedConfiguration()) {
      return Status::kUnsupported;
    }
    const size_t pitch = (parameters_.mfx.FrameInfo.Width + 31u) & ~31u;
    pixel_bytes_ = pitch * parameters_.mfx.FrameInfo.Height * 3 / 2;
    const size_t buffer_hint =
        static_cast<size_t>(parameters_.mfx.BufferSizeInKB) * 1000 *
        std::max<mfxU16>(1, parameters_.mfx.BRCParamMultiplier);
    const size_t output_bytes =
        std::max(buffer_hint, pixel_bytes_ * 2 + 1024 * 1024);
    if (pixel_bytes_ > kMaximumBuffer || output_bytes > kMaximumBuffer) {
      return Fail(Status::kUnsupported, "QSV required buffers exceed limit");
    }
    pixels_.reset(static_cast<uint8_t*>(_aligned_malloc(pixel_bytes_, 32)));
    output_.reset(static_cast<uint8_t*>(_aligned_malloc(output_bytes, 32)));
    if (!pixels_ || !output_) {
      return Fail(Status::kError, "QSV buffer allocation failed");
    }
    surface_ = {};
    surface_.Info = parameters_.mfx.FrameInfo;
    surface_.Data.Pitch = static_cast<mfxU16>(pitch);
    surface_.Data.Y = pixels_.get();
    surface_.Data.UV = pixels_.get() + pitch * surface_.Info.Height;
    bitstream_ = {};
    bitstream_.Data = output_.get();
    bitstream_.MaxLength = static_cast<mfxU32>(output_bytes);
    initialized_ = true;
    need_idr_ = true;
    RTC_LOG(LS_INFO) << "QSV " << diagnostic_;
    return Status::kOk;
  }

  static bool AcceptConfigurationStatus(mfxStatus status) {
    return status == MFX_ERR_NONE || status == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM;
  }
  void SetRateFields() {
    parameters_.mfx.BRCParamMultiplier = 1;
    parameters_.mfx.TargetKbps =
        static_cast<mfxU16>((config_.bitrate_bps + 999) / 1000);
    parameters_.mfx.MaxKbps = parameters_.mfx.TargetKbps;
    parameters_.mfx.FrameInfo.FrameRateExtN = config_.framerate_fps;
    parameters_.mfx.FrameInfo.FrameRateExtD = 1;
  }
  void MakeParameters() {
    options_ = {};
    options_.Header = {MFX_EXTBUFF_CODING_OPTION, sizeof(options_)};
    options2_ = {};
    options2_.Header = {MFX_EXTBUFF_CODING_OPTION2, sizeof(options2_)};
    options2_.AdaptiveI = MFX_CODINGOPTION_OFF;
    options2_.AdaptiveB = MFX_CODINGOPTION_OFF;
    options2_.ExtBRC = MFX_CODINGOPTION_OFF;
    extensions_.clear();
    if (codec_ == Codec::kH264) {
      // These two controls are AVC-specific; HEVC always uses CABAC and obtains
      // the complete active VPS/SPS/PPS separately after initialization/reset.
      options_.CAVLC = MFX_CODINGOPTION_ON;
      options2_.RepeatPPS = MFX_CODINGOPTION_ON;
      options2_.BRefType = MFX_B_REF_OFF;
      extensions_.push_back(&options_.Header);
    }
    extensions_.push_back(&options2_.Header);
    hevc_ = {};
    if (codec_ == Codec::kH265 && (config_.width % 16 || config_.height % 16)) {
      // Follow the SDK/OBS pattern: storage remains aligned to 16, while the
      // SDK may pad the requested coded size and use an SPS conformance crop.
      hevc_.Header = {MFX_EXTBUFF_HEVC_PARAM, sizeof(hevc_)};
      hevc_.PicWidthInLumaSamples = static_cast<mfxU16>(config_.width);
      hevc_.PicHeightInLumaSamples = static_cast<mfxU16>(config_.height);
      extensions_.push_back(&hevc_.Header);
    }
    parameters_ = {};
    parameters_.ExtParam = extensions_.data();
    parameters_.NumExtParam = static_cast<mfxU16>(extensions_.size());
    parameters_.AsyncDepth = 1;
    parameters_.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    auto& info = parameters_.mfx;
    info.CodecId = codec_ == Codec::kH264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;
    info.CodecProfile = codec_ == Codec::kH264 ? MFX_PROFILE_AVC_BASELINE
                                               : MFX_PROFILE_HEVC_MAIN;
    // oneVPL uses 10 times the HEVC level, unlike RTP/SPS's 30-times level_idc.
    // Main tier is encoded as zero; the High-tier flag must never be present.
    info.CodecLevel =
        codec_ == Codec::kH264 ? config_.h264_level : config_.h265_level / 3;
    info.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    info.RateControlMethod = MFX_RATECONTROL_CBR;
    info.GopPicSize = 65535;
    info.GopRefDist = 1;
    info.GopOptFlag = MFX_GOP_CLOSED;
    info.NumRefFrame = 1;
    if (codec_ == Codec::kH265) {
      // HEVC uses one for every I picture to be IDR (AVC uses zero).
      info.IdrInterval = 1;
      info.FrameInfo.BitDepthLuma = 8;
      info.FrameInfo.BitDepthChroma = 8;
    }
    info.FrameInfo.FourCC = MFX_FOURCC_NV12;
    info.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    info.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    info.FrameInfo.Width = static_cast<mfxU16>((config_.width + 15) & ~15);
    info.FrameInfo.Height = static_cast<mfxU16>((config_.height + 15) & ~15);
    info.FrameInfo.CropW = static_cast<mfxU16>(config_.width);
    info.FrameInfo.CropH = static_cast<mfxU16>(config_.height);
    SetRateFields();
  }
  void LogConfiguration(const char* stage,
                        mfxStatus status,
                        const mfxVideoParam& actual,
                        const mfxExtHEVCParam* hevc) const {
    if (codec_ != Codec::kH265) {
      return;
    }
    const auto& info = actual.mfx;
    const auto& frame = info.FrameInfo;
    std::ostringstream text;
    text << "QSV_CONFIG stage=" << stage << " vendorStatus=" << status
         << " requested{display=" << config_.width << 'x' << config_.height
         << " profile=Main tier=0 level_idc="
         << static_cast<unsigned>(config_.h265_level)
         << " sdk_level=" << static_cast<unsigned>(config_.h265_level / 3)
         << " async=1 system_memory=1 gop_ref=1 encoded_order=0 idr_interval=1"
         << " cbr_kbps=" << (config_.bitrate_bps + 999) / 1000
         << " fps=" << config_.framerate_fps << " NV12_8bit=1}"
         << " returned{codec_id=0x" << std::hex << info.CodecId << " profile=0x"
         << info.CodecProfile << " sdk_level=0x" << info.CodecLevel << std::dec
         << " async=" << actual.AsyncDepth << " io_pattern=" << actual.IOPattern
         << " gop_ref=" << info.GopRefDist
         << " encoded_order=" << info.EncodedOrder
         << " idr_interval=" << info.IdrInterval
         << " rc_method=" << info.RateControlMethod
         << " target_kbps=" << info.TargetKbps << " max_kbps=" << info.MaxKbps
         << " buffer_kb=" << info.BufferSizeInKB
         << " initial_delay_kb=" << info.InitialDelayInKB
         << " brc_multiplier=" << info.BRCParamMultiplier << " effective_kbps="
         << static_cast<uint64_t>(info.TargetKbps) *
                std::max<mfxU16>(1, info.BRCParamMultiplier)
         << " fps=" << frame.FrameRateExtN << '/' << frame.FrameRateExtD
         << " fourcc=0x" << std::hex << frame.FourCC << std::dec
         << " chroma=" << frame.ChromaFormat
         << " pic_struct=" << frame.PicStruct << " storage=" << frame.Width
         << 'x' << frame.Height << " crop=" << frame.CropX << ',' << frame.CropY
         << ',' << frame.CropW << ',' << frame.CropH
         << " depth=" << frame.BitDepthLuma << '/' << frame.BitDepthChroma
         << " shift=" << frame.Shift;
    if (hevc && hevc->Header.BufferId) {
      text << " hevc_header_size=" << hevc->Header.BufferSz
           << " hevc_coded=" << hevc->PicWidthInLumaSamples << 'x'
           << hevc->PicHeightInLumaSamples;
    }
    text << '}';
    RTC_LOG(LS_INFO) << text.str();
  }

  bool ReadValidatedConfiguration() {
    // HEVC's extension describes coded dimensions, while the SPS conformance
    // window establishes the displayed size. Bind both before accepting the
    // SDK readback; preserve the previously validated AVC call order.
    if (codec_ == Codec::kH265) {
      return ReadParameterSets() && ReadBackConfiguration();
    }
    return ReadBackConfiguration() && ReadParameterSets();
  }

  bool ReadBackConfiguration() {
    mfxVideoParam actual = {};
    mfxExtHEVCParam actual_hevc = {};
    mfxExtBuffer* hevc_extension = &actual_hevc.Header;
    if (codec_ == Codec::kH265 && hevc_.Header.BufferId) {
      actual_hevc.Header = {MFX_EXTBUFF_HEVC_PARAM, sizeof(actual_hevc)};
      actual.ExtParam = &hevc_extension;
      actual.NumExtParam = 1;
    }
    const auto result = MFXVideoENCODE_GetVideoParam(session_, &actual);
    LogConfiguration("GetVideoParam", result, actual, &actual_hevc);
    const auto& info = actual.mfx;
    const auto& frame = info.FrameInfo;
    std::vector<std::string> mismatches;
    const auto require = [&](bool valid, const char* field) {
      if (!valid) {
        mismatches.emplace_back(field);
      }
    };
    require(result == MFX_ERR_NONE, "vendorStatus");
    require(actual.AsyncDepth == 1, "AsyncDepth");
    require(actual.IOPattern == MFX_IOPATTERN_IN_SYSTEM_MEMORY, "IOPattern");
    if (codec_ == Codec::kH264) {
      require(info.CodecId == MFX_CODEC_AVC, "CodecId");
      require(info.CodecProfile == MFX_PROFILE_AVC_BASELINE, "CodecProfile");
      require(info.CodecLevel != 0 && info.CodecLevel <= config_.h264_level,
              "CodecLevel");
    } else {
      require(info.CodecId == MFX_CODEC_HEVC, "CodecId");
      require(info.CodecProfile == MFX_PROFILE_HEVC_MAIN, "CodecProfile");
      require((info.CodecLevel & MFX_TIER_HEVC_HIGH) == 0, "HEVC_tier");
      require(info.CodecLevel != 0 &&
                  static_cast<uint32_t>(info.CodecLevel) * 3 <=
                      config_.h265_level &&
                  ValidH265Level(static_cast<uint8_t>(info.CodecLevel * 3)),
              "CodecLevel");
      require(info.IdrInterval == 1, "IdrInterval");
#if defined(RTC_ENABLE_H265)
      const auto dimensions = ParseQsvHevcSpsDimensions(sps_);
      const uint32_t coded_width = hevc_.Header.BufferId
                                       ? actual_hevc.PicWidthInLumaSamples
                                   : dimensions ? dimensions->coded_width
                                                : 0;
      const uint32_t coded_height = hevc_.Header.BufferId
                                        ? actual_hevc.PicHeightInLumaSamples
                                    : dimensions ? dimensions->coded_height
                                                 : 0;
      // Aligned sessions do not request an HEVC extension. In that case the
      // active SPS supplies the coded size; all crop/storage checks still
      // apply.
      require(dimensions && QsvHevcDimensionsMatch(*dimensions, config_.width,
                                                   config_.height, frame.Width,
                                                   frame.Height, coded_width,
                                                   coded_height),
              "HEVC_SPS_coded_display_dimensions");
      if (dimensions) {
        RTC_LOG(LS_INFO) << "QSV_CONFIG active_SPS coded="
                         << dimensions->coded_width << "x"
                         << dimensions->coded_height
                         << " display=" << dimensions->display_width << "x"
                         << dimensions->display_height;
      }
#else
      require(false, "HEVC_SPS_parser_unavailable");
#endif
    }
    require(info.GopRefDist == 1, "GopRefDist");
    require(info.EncodedOrder == 0, "EncodedOrder");
    require(info.RateControlMethod == MFX_RATECONTROL_CBR, "RateControlMethod");
    require(frame.FourCC == MFX_FOURCC_NV12, "FourCC");
    require(frame.ChromaFormat == MFX_CHROMAFORMAT_YUV420, "ChromaFormat");
    require(frame.PicStruct == MFX_PICSTRUCT_PROGRESSIVE, "PicStruct");
    require(frame.CropX == 0 && frame.CropY == 0, "CropXY");
    require(frame.CropW == config_.width && frame.CropH == config_.height,
            "CropWH");
    require(frame.Width >= frame.CropW && frame.Height >= frame.CropH &&
                frame.Width <= 8192 && frame.Height <= 8192 &&
                frame.Width % 16 == 0 && frame.Height % 16 == 0,
            "StorageWH");
    require(
        frame.BitDepthLuma <= 8 && frame.BitDepthChroma <= 8 && !frame.Shift,
        "BitDepthShift");
    require(frame.FrameRateExtD != 0 &&
                frame.FrameRateExtN ==
                    static_cast<uint64_t>(config_.framerate_fps) *
                        frame.FrameRateExtD,
            "FrameRate");
    require(static_cast<uint64_t>(info.TargetKbps) *
                    std::max<mfxU16>(1, info.BRCParamMultiplier) ==
                (config_.bitrate_bps + 999) / 1000,
            "TargetKbpsMultiplier");
    if (!mismatches.empty()) {
      std::ostringstream reason;
      reason << "QSV configuration readback vendorStatus=" << result
             << " mismatch=";
      for (size_t i = 0; i < mismatches.size(); ++i) {
        reason << (i ? "," : "") << mismatches[i];
      }
      Fail(Status::kUnsupported, reason.str());
      return false;
    }
    parameters_ = actual;
    parameters_.ExtParam = extensions_.data();
    parameters_.NumExtParam = static_cast<mfxU16>(extensions_.size());
    if (actual_hevc.Header.BufferId) {
      hevc_ = actual_hevc;
    }
    return true;
  }
  bool ReadParameterSets() {
    std::array<mfxU8, 16384> sps;
    std::array<mfxU8, 16384> pps;
    std::array<mfxU8, 16384> vps;
    mfxExtCodingOptionSPSPPS headers = {};
    headers.Header = {MFX_EXTBUFF_CODING_OPTION_SPSPPS, sizeof(headers)};
    headers.SPSBuffer = sps.data();
    headers.PPSBuffer = pps.data();
    headers.SPSBufSize = static_cast<mfxU16>(sps.size());
    headers.PPSBufSize = static_cast<mfxU16>(pps.size());
    mfxExtCodingOptionVPS vps_header = {};
    vps_header.Header = {MFX_EXTBUFF_CODING_OPTION_VPS, sizeof(vps_header)};
    vps_header.VPSBuffer = vps.data();
    vps_header.VPSBufSize = static_cast<mfxU16>(vps.size());
    std::array<mfxExtBuffer*, 2> extensions = {&headers.Header,
                                               &vps_header.Header};
    mfxVideoParam actual = {};
    actual.ExtParam = extensions.data();
    actual.NumExtParam = codec_ == Codec::kH264 ? 1 : 2;
    const auto result = MFXVideoENCODE_GetVideoParam(session_, &actual);
    if (result != MFX_ERR_NONE || headers.SPSBufSize > sps.size() ||
        headers.PPSBufSize > pps.size() ||
        !SaveParameterSet(codec_, sps.data(), headers.SPSBufSize,
                          codec_ == Codec::kH264 ? 7 : 33, &sps_) ||
        !SaveParameterSet(codec_, pps.data(), headers.PPSBufSize,
                          codec_ == Codec::kH264 ? 8 : 34, &pps_) ||
        (codec_ == Codec::kH265 &&
         (vps_header.VPSBufSize > vps.size() ||
          !SaveParameterSet(codec_, vps.data(), vps_header.VPSBufSize, 32,
                            &vps_)))) {
      Fail(Status::kUnsupported,
           codec_ == Codec::kH264 ? "QSV active SPS/PPS retrieval"
                                  : "QSV active VPS/SPS/PPS retrieval",
           result);
      return false;
    }
    return true;
  }

  const Codec codec_;
  Config config_;
  std::unique_ptr<QsvSessionResources> resources_ =
      std::make_unique<QsvSessionResources>();
  mfxLoader& loader_ = resources_->loader;
  mfxSession& session_ = resources_->session;
  ComPtr<ID3D11Device>& device_ = resources_->device;
  mfxVideoParam& parameters_ = resources_->parameters;
  mfxExtCodingOption& options_ = resources_->options;
  mfxExtCodingOption2& options2_ = resources_->options2;
  mfxExtHEVCParam& hevc_ = resources_->hevc;
  std::vector<mfxExtBuffer*>& extensions_ = resources_->extensions;
  mfxFrameSurface1& surface_ = resources_->surface;
  mfxBitstream& bitstream_ = resources_->bitstream;
  mfxEncodeCtrl& control_ = resources_->control;
  mfxSyncPoint& pending_ = resources_->pending;
  AlignedBytes& pixels_ = resources_->pixels;
  AlignedBytes& output_ = resources_->output;
  size_t pixel_bytes_ = 0;
  std::vector<uint8_t> vps_;
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
  bool initialized_ = false;
  bool encoder_initialized_ = false;
  bool need_idr_ = true;
  bool retirement_unconfirmed_ = false;
  std::string error_;
  std::string diagnostic_;
  Status last_close_status_ = Status::kOk;
};

}  // namespace

std::unique_ptr<VideoEncoderBackend> CreateQsvH264Backend() {
  return std::make_unique<QsvBackend>(Codec::kH264);
}

std::unique_ptr<VideoEncoderBackend> CreateQsvH265Backend() {
  return std::make_unique<QsvBackend>(Codec::kH265);
}

namespace {

Status ProbeQsvBackend(Codec codec, std::string* reason, uint8_t level) {
  QsvBackend backend(codec);
  Config config;
  config.codec = codec;
  config.width = codec == Codec::kH264 ? 176 : 160;
  config.height = codec == Codec::kH264 ? 144 : 128;
  config.bitrate_bps = 64000;
  config.framerate_fps = 15;
  if (codec == Codec::kH264) {
    config.h264_level = level;
  } else {
    config.h265_level = level;
  }
  auto status = backend.Open(config);
  backend.Close();
  if (backend.LastCloseStatus() != Status::kOk) {
    status = Status::kRetirementFailed;
  }
  if (reason) {
    *reason = status == Status::kOk ? backend.Diagnostic()
                                    : std::string(backend.LastError());
  }
  return status;
}

}  // namespace

Status ProbeQsvH264Backend(std::string* reason, uint8_t h264_level) {
  return HardwareEncoderProbes().Run(
      HardwareEncoderVendor::kQsv, reason,
      [h264_level](std::string* diagnostic) {
        return ProbeQsvBackend(Codec::kH264, diagnostic, h264_level);
      });
}

Status ProbeQsvH265Backend(std::string* reason, uint8_t h265_level) {
  return HardwareEncoderProbes().Run(
      HardwareEncoderVendor::kQsv, reason,
      [h265_level](std::string* diagnostic) {
        return ProbeQsvBackend(Codec::kH265, diagnostic, h265_level);
      });
}

}  // namespace webrtc::external_encoder
