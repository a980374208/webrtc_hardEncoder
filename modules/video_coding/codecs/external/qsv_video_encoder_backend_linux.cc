/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/qsv_video_encoder_backend_linux.h"
#include "modules/video_coding/codecs/external/linux_qsv_driver.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace webrtc::external_encoder {
namespace {
constexpr uint16_t kIntelVendor = 0x8086;
constexpr size_t kMaximumDevices = 128;
constexpr size_t kMaximumBuffer = 128 * 1024 * 1024;
constexpr size_t kMaximumSurfaces = 16;
// This is the driver's cooperative sync budget. The outer Linux bounded
// worker remains necessary: a driver may ignore the wait argument entirely.
constexpr mfxU32 kSyncWaitMs = 200;
struct Device {
  std::string node;
  uint16_t device = 0, domain = 0;
  uint8_t bus = 0, slot = 0, function = 0;
};
bool IntelDevice(const drmDevice* device) {
  return device && device->bustype == DRM_BUS_PCI && device->businfo.pci &&
      device->deviceinfo.pci && device->deviceinfo.pci->vendor_id == kIntelVendor;
}
bool SameDevice(const Device& expected, const drmDevice* actual) {
  return IntelDevice(actual) && actual->deviceinfo.pci->device_id == expected.device &&
      actual->businfo.pci->domain == expected.domain && actual->businfo.pci->bus == expected.bus &&
      actual->businfo.pci->dev == expected.slot && actual->businfo.pci->func == expected.function;
}
bool SameDevice(const Device& expected, const mfxExtendedDeviceId& actual) {
  return actual.VendorID == kIntelVendor && actual.DeviceID == expected.device &&
      actual.PCIDomain == expected.domain && actual.PCIBus == expected.bus &&
      actual.PCIDevice == expected.slot && actual.PCIFunction == expected.function;
}
bool ValidRates(const Rates& rates) {
  return rates.bitrate_bps > 0 && rates.bitrate_bps <= 65535000 &&
      rates.framerate_fps > 0 && rates.framerate_fps <= 240;
}
bool ValidLevel(uint8_t level) {
  constexpr uint8_t values[] = {10,11,12,13,20,21,22,30,31,32,40,41,42,50,51,52,60,61,62};
  return std::find(std::begin(values), std::end(values), level) != std::end(values);
}
bool Accepted(mfxStatus status) {
  return status == MFX_ERR_NONE || status == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM;
}
bool HasNal(const uint8_t* data, size_t size, uint8_t type) {
  for (size_t i = 0; i + 3 < size; ++i) {
    if (data[i] || data[i + 1]) continue;
    size_t prefix = data[i + 2] == 1 ? 3 : 0;
    if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) prefix = 4;
    if (prefix && i + prefix < size && (data[i + prefix] & 0x80) == 0 &&
        (data[i + prefix] & 0x1f) == type) return true;
  }
  return false;
}
bool SaveHeader(const uint8_t* data, size_t size, uint8_t type,
                std::vector<uint8_t>* output) {
  output->clear();
  if (!size) return false;
  if (HasNal(data, size, type)) { output->assign(data, data + size); return true; }
  if ((data[0] & 0x80) || (data[0] & 0x1f) != type) return false;
  *output = {0,0,0,1}; output->insert(output->end(), data, data + size); return true;
}
struct FreeBytes { void operator()(uint8_t* bytes) const { free(bytes); } };
using AlignedBytes = std::unique_ptr<uint8_t, FreeBytes>;
bool Allocate(size_t size, AlignedBytes* result) {
  void* storage = nullptr;
  if (posix_memalign(&storage, 32, size) != 0) return false;
  result->reset(static_cast<uint8_t*>(storage)); return true;
}
bool Multiply(size_t a, size_t b, size_t* result) {
  if (b && a > std::numeric_limits<size_t>::max() / b) return false;
  *result = a * b;
  return true;
}
bool Add(size_t a, size_t b, size_t* result) {
  if (a > std::numeric_limits<size_t>::max() - b) return false;
  *result = a + b;
  return true;
}
struct OwnedSurface {
  mfxFrameSurface1 frame{};
  AlignedBytes pixels;
};
// All asynchronous addresses, dispatch DSOs and display dependencies remain
// in this stable heap block through MFXClose. Never free it on failed abort.
struct Resources {
  explicit Resources(std::shared_ptr<internal::LinuxQsvDriver> value)
      : driver(std::move(value)) {}
  ~Resources() {
    if (pool_bytes)
      RecordLinuxEncoderPoolReleased(LinuxEncoderMetricBackend::kQsv,
          surfaces.size(), pool_bytes, pool_retained);
  }
  void RetainPool() {
    if (pool_bytes && !pool_retained) {
      pool_retained = true;
      RecordLinuxEncoderPoolRetained(LinuxEncoderMetricBackend::kQsv,
          surfaces.size(), pool_bytes);
    }
  }
  std::shared_ptr<internal::LinuxQsvDriver> driver;
  int fd = -1;
  VADisplay display = nullptr;
  bool va_initialized = false;
  uint16_t va_major = 0, va_minor = 0, vpl_major = 0, vpl_minor = 0;
  mfxLoader loader = nullptr;
  mfxHDL description = nullptr, extended_device = nullptr;
  mfxSession session = nullptr;
  bool encoder_initialized = false;
  mfxVideoParam parameters{};
  mfxExtCodingOption options{};
  mfxExtCodingOption2 options2{};
  std::array<mfxExtBuffer*, 2> extensions{};
  std::vector<OwnedSurface> surfaces;
  // Actual owned input-surface allocation, including alignment/padding. The
  // output bitstream buffer is separate and is not a CPU surface-pool metric.
  size_t pool_bytes = 0;
  bool pool_retained = false;
  size_t next_surface = 0;
  mfxBitstream bitstream{};
  mfxEncodeCtrl control{};
  mfxSyncPoint pending = nullptr;
  AlignedBytes output;
};

class LinuxQsvBackend final : public VideoEncoderBackend {
 public:
  explicit LinuxQsvBackend(int ordinal, std::shared_ptr<internal::LinuxQsvDriver> driver = {},
      std::optional<LinuxQsvDeviceSelection> expected = {},
      std::shared_ptr<internal::LinuxQsvDeviceCollector> collector = {})
      : ordinal_(ordinal), expected_(std::move(expected)), collector_(std::move(collector)),
        injected_(std::move(driver)) {
    RecordLinuxEncoderBackendCreated(LinuxEncoderMetricBackend::kQsv);
  }
  ~LinuxQsvBackend() override {
    Close();
    if (retirement_unconfirmed_) (void)resources_.release();
  }
  Info GetInfo() const override { return {"QSV-VAAPI-Linux", true}; }
  Status LastCloseStatus() const override { return last_close_status_; }
  std::string_view LastError() const override { return error_; }
  Status Open(const Config& config) override {
    RecordLinuxEncoderBackendOpenAttempt(LinuxEncoderMetricBackend::kQsv);
    Close();
    if (last_close_status_ != Status::kOk ||
        HardwareEncoderProbes().Check(HardwareEncoderVendor::kQsv) == Status::kRetirementFailed)
      return Fail(Status::kRetirementFailed, "qsv_retirement_unconfirmed");
    error_.clear(); unhealthy_ = false; has_selected_device_ = false;
    if (collector_) collector_->BeginOpen();
    if (expected_ && !ValidSelection(*expected_))
      return Fail(Status::kInvalidArgument, "invalid_qsv_device_selection");
    if (config.codec != Codec::kH264) return Fail(Status::kUnsupported, "qsv_h264_baseline_only");
    if (ordinal_ < -1 || config.width <= 0 || config.height <= 0 ||
        config.width > 8192 || config.height > 8192 || (config.width & 1) ||
        (config.height & 1) || !ValidLevel(config.h264_level) ||
        !ValidRates({config.bitrate_bps, config.framerate_fps}))
      return Fail(Status::kInvalidArgument, "invalid_qsv_configuration");
    config_ = config;
    Status status = Status::kOk;
    auto driver = injected_ ? injected_ : internal::LoadLinuxQsvDriver(&status, &error_);
    if (!driver) return status;
    if (!driver->HasRequiredFunctions()) return Fail(Status::kUnavailable, "qsv_function_table_incomplete");
    if (expected_) {
      const auto& selected = *expected_;
      const Device device{selected.render_node, selected.device_id,
          static_cast<uint16_t>(selected.pci_domain), selected.pci_bus,
          selected.pci_device, selected.pci_function};
      resources_ = std::make_unique<Resources>(driver);
      status = OpenDevice(device);
      if (status == Status::kOk) {
        Selected(device);
        RecordLinuxEncoderBackendOpenSucceeded(LinuxEncoderMetricBackend::kQsv);
        return status;
      }
      Close();
      return last_close_status_ == Status::kOk ? status : Status::kRetirementFailed;
    }
    std::array<drmDevicePtr, kMaximumDevices> handles{};
    const int count = driver->get_devices(0, handles.data(), handles.size());
    if (count < 0) return Fail(Status::kUnavailable, "qsv_drm_enumeration_unavailable");
    std::vector<Device> devices;
    for (int i = 0; i < std::min<int>(count, handles.size()); ++i) {
      const auto* item = handles[i];
      if (!IntelDevice(item) || !(item->available_nodes & (1 << DRM_NODE_RENDER)) ||
          !item->nodes || !item->nodes[DRM_NODE_RENDER]) continue;
      const char* node = item->nodes[DRM_NODE_RENDER];
      const size_t length = strnlen(node, 4097);
      constexpr char prefix[] = "/dev/dri/renderD";
      if (length > 4096 || length <= sizeof(prefix) - 1 ||
          std::strncmp(node, prefix, sizeof(prefix) - 1) != 0 ||
          !std::all_of(node + sizeof(prefix) - 1, node + length,
                       [](char c) { return c >= '0' && c <= '9'; })) continue;
      devices.push_back({node, item->deviceinfo.pci->device_id, item->businfo.pci->domain,
          item->businfo.pci->bus, item->businfo.pci->dev, item->businfo.pci->func});
    }
    driver->free_devices(handles.data(), std::min<int>(count, handles.size()));
    if (count > static_cast<int>(handles.size())) return Fail(Status::kUnsupported, "qsv_drm_device_limit");
    std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) {
      return std::tie(a.domain,a.bus,a.slot,a.function,a.node) < std::tie(b.domain,b.bus,b.slot,b.function,b.node);
    });
    devices.erase(std::unique(devices.begin(), devices.end(), [](const Device& a, const Device& b) {
      return std::tie(a.domain,a.bus,a.slot,a.function,a.node) == std::tie(b.domain,b.bus,b.slot,b.function,b.node);
    }), devices.end());
    if (devices.empty() || ordinal_ >= static_cast<int>(devices.size()))
      return Fail(Status::kUnavailable, "qsv_intel_render_device_unavailable");
    const size_t first = ordinal_ < 0 ? 0 : static_cast<size_t>(ordinal_);
    const size_t limit = ordinal_ < 0 ? devices.size() : first + 1;
    for (size_t i = first; i < limit; ++i) {
      resources_ = std::make_unique<Resources>(driver);
      status = OpenDevice(devices[i]);
      if (status == Status::kOk) {
        Selected(devices[i]);
        RecordLinuxEncoderBackendOpenSucceeded(LinuxEncoderMetricBackend::kQsv);
        return status;
      }
      Close();
      if (last_close_status_ != Status::kOk) return Status::kRetirementFailed;
    }
    return status;
  }
  Status SetRates(const Rates& rates) override {
    if (!Ready()) return Fail(Status::kError, "qsv_not_initialized");
    if (!ValidRates(rates)) return Fail(Status::kInvalidArgument, "invalid_qsv_rates");
    auto& r = *resources_;
    if (r.pending) return Abort(Status::kError, "qsv_reset_with_pending_frame");
    // Completed output can leave valid reference surfaces Locked. They are not
    // pending output. Conservatively retire this session and rebuild on the
    // same captured device, keeping every old surface alive until MFXClose.
    if (AnyLocked()) return ReopenForRates(rates);
    config_.bitrate_bps = rates.bitrate_bps; config_.framerate_fps = rates.framerate_fps;
    RateFields();
    if (!Accepted(r.driver->reset(r.session, &r.parameters))) return Abort(Status::kUnsupported, "qsv_reset_unsupported");
    if (!ReadParameters() || r.pending)
      return Abort(Status::kUnsupported, "qsv_reset_readback_or_ownership_mismatch");
    if (AnyLocked()) return ReopenForRates(rates);
    size_t required = 0;
    if (!PoolRequirement(&required) || !PrepareStorage(required))
      return Abort(Status::kUnsupported, "qsv_reset_pool_requirement_unsupported");
    need_idr_ = true; return Status::kOk;
  }
  Status Encode(const Frame& frame, bool force, Packet* packet) override {
    if (packet) *packet = {};
    if (!Ready()) return Fail(Status::kError, "qsv_not_initialized");
    auto& r = *resources_;
    if (r.pending) return Abort(Status::kError, "qsv_input_not_retired");
    if (!packet || !frame.y || !frame.uv || frame.width != config_.width ||
        frame.height != config_.height || frame.stride_y < frame.width || frame.stride_uv < frame.width ||
        frame.id == static_cast<uint64_t>(MFX_TIMESTAMP_UNKNOWN))
      return Fail(Status::kInvalidArgument, "invalid_qsv_frame");
    auto* owned = AcquireSurface();
    if (!owned) return Abort(Status::kError, "qsv_no_unlocked_surface");
    auto& surface = owned->frame;
    LinuxEncoderTransferTimer cpu_copy(LinuxEncoderMetricTransfer::kQsvCpuSurfaceCopy,
        static_cast<uint64_t>(frame.width) * frame.height * 3 / 2);
    const size_t y_size = static_cast<size_t>(surface.Data.Pitch) * surface.Info.Height;
    std::memset(owned->pixels.get(), 0, y_size);
    std::memset(owned->pixels.get() + y_size, 128, y_size / 2);
    for (int row = 0; row < frame.height; ++row)
      std::memcpy(surface.Data.Y + static_cast<size_t>(row) * surface.Data.Pitch,
          frame.y + static_cast<size_t>(row) * frame.stride_y, frame.width);
    for (int row = 0; row < frame.height / 2; ++row)
      std::memcpy(surface.Data.UV + static_cast<size_t>(row) * surface.Data.Pitch,
          frame.uv + static_cast<size_t>(row) * frame.stride_uv, frame.width);
    cpu_copy.Finish(true);
    surface.Data.TimeStamp = frame.id;
    surface.Data.FrameOrder = static_cast<mfxU32>(frame.id);
    r.bitstream.DataOffset = 0; r.bitstream.DataLength = 0;
    r.bitstream.TimeStamp = MFX_TIMESTAMP_UNKNOWN; r.bitstream.FrameType = 0;
    r.control = {};
    const bool key = force || need_idr_;
    if (key) r.control.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
    // Exactly one submission, never a busy retry loop or a delayed-frame queue.
    const auto submitted = r.driver->encode(r.session, &r.control, &surface, &r.bitstream, &r.pending);
    if (submitted == MFX_WRN_DEVICE_BUSY && !r.pending && !surface.Data.Locked)
      return Abort(Status::kUnavailable, "qsv_device_busy");
    if (submitted == MFX_ERR_MORE_DATA || submitted == MFX_ERR_MORE_SURFACE)
      return Abort(Status::kUnsupported, "qsv_delayed_output_unsupported");
    if (submitted != MFX_ERR_NONE || !r.pending) return Abort(Status::kError, "qsv_submission_failed");
    const auto synced = r.driver->sync(r.session, r.pending, kSyncWaitMs);
    if (synced == MFX_WRN_IN_EXECUTION) {
      // Even a successful later abort must not silently turn this timeout into
      // an ordinary fallback. Cleanup runs before the permanent poison.
      Abort(Status::kError, "qsv_sync_timeout");
      RecordFailure("qsv_sync_timeout");
      return Status::kRetirementFailed;
    }
    if (synced != MFX_ERR_NONE) return Abort(Status::kError, "qsv_sync_failed");
    r.pending = nullptr;
    // Sync confirms this packet, but the encoder may still retain a reference
    // to the input surface. Its owned pixels remain in the pool until unlocked
    // or confirmed session abort; subsequent calls select another free surface.
    if (r.bitstream.TimeStamp != frame.id || !r.bitstream.DataLength ||
        r.bitstream.DataOffset > r.bitstream.MaxLength ||
        r.bitstream.DataLength > r.bitstream.MaxLength - r.bitstream.DataOffset)
      return Abort(Status::kError, "qsv_output_ownership_timestamp_size");
    const auto* data = r.bitstream.Data + r.bitstream.DataOffset;
    const size_t size = r.bitstream.DataLength;
    const bool idr = HasNal(data, size, 5);
    if ((!idr && !HasNal(data, size, 1)) || (key && !idr))
      return Abort(Status::kError, "qsv_annex_b_picture_mismatch");
    if (idr && (!HasNal(data, size, 7) || !HasNal(data, size, 8))) {
      packet->bytes.insert(packet->bytes.end(), sps_.begin(), sps_.end());
      packet->bytes.insert(packet->bytes.end(), pps_.begin(), pps_.end());
    }
    packet->bytes.insert(packet->bytes.end(), data, data + size);
    packet->frame_id = frame.id; packet->key_frame = idr; need_idr_ = false;
    return Status::kOk;
  }
  void Close() override {
    initialized_ = false;
    if (!resources_ || retirement_unconfirmed_) return;
    auto& r = *resources_; auto& d = *r.driver;
    if (r.session) {
      if (r.pending) d.sync(r.session, r.pending, 10);  // Best effort; MFXClose aborts.
      const auto encoder = d.close_encoder(r.session);
      if (encoder != MFX_ERR_NONE && !(encoder == MFX_ERR_NOT_INITIALIZED && !r.encoder_initialized))
        RecordFailure("qsv_encoder_close_failed");
      if (d.close_session(r.session) != MFX_ERR_NONE) {
        RetainResources("qsv_session_close_failed"); return;
      }
      r.session = nullptr; r.pending = nullptr;
    }
    if (r.description && d.release_description(r.loader, r.description) != MFX_ERR_NONE) {
      RetainResources("qsv_description_release_failed"); return;
    }
    r.description = nullptr;
    if (r.extended_device && d.release_description(r.loader, r.extended_device) != MFX_ERR_NONE) {
      RetainResources("qsv_device_description_release_failed"); return;
    }
    r.extended_device = nullptr;
    if (r.loader) { d.unload(r.loader); r.loader = nullptr; }
    if (r.va_initialized) {
      if (d.terminate_va(r.display) != VA_STATUS_SUCCESS) {
        RetainResources("qsv_va_terminate_failed"); return;
      }
      r.va_initialized = false;
    }
    if (r.fd >= 0) {
      if (d.close_node(r.fd) != 0) {
        RetainResources("qsv_render_node_close_failed"); return;
      }
      r.fd = -1;
    }
    resources_.reset(); sps_.clear(); pps_.clear();
    if (collector_ && last_close_status_ == Status::kOk) collector_->Closed();
  }
 private:
  bool Ready() const { return initialized_ && resources_ && !unhealthy_ && !retirement_unconfirmed_; }
  Status Fail(Status status, const char* reason) {
    if (last_close_status_ == Status::kOk) error_ = reason;
    return last_close_status_ == Status::kOk ? status : Status::kRetirementFailed;
  }
  void RetainResources(const char* reason) {
    retirement_unconfirmed_ = true;
    if (resources_) resources_->RetainPool();
    RecordFailure(reason);
  }
  void RecordFailure(const char* reason) {
    last_close_status_ = Status::kRetirementFailed; error_ = reason;
    if (collector_) collector_->FailedRetirement();
    HardwareEncoderProbes().RecordRetirementFailure(HardwareEncoderVendor::kQsv, reason);
  }
  Status Abort(Status status, const char* reason) {
    unhealthy_ = true; Fail(status, reason); Close();
    return last_close_status_ == Status::kOk ? status : Status::kRetirementFailed;
  }
  bool Filter(const char* name, mfxU32 value) {
    auto& r = *resources_; mfxVariant variant{};
    variant.Version.Version = MFX_VARIANT_VERSION;
    variant.Type = MFX_VARIANT_TYPE_U32; variant.Data.U32 = value;
    const auto filter = r.driver->create_config(r.loader);
    return filter && r.driver->filter(filter, reinterpret_cast<const mfxU8*>(name), variant) == MFX_ERR_NONE;
  }
  bool ReleaseDescriptions() {
    auto& r = *resources_;
    if (r.description) {
      if (r.driver->release_description(r.loader, r.description) != MFX_ERR_NONE) return false;
      r.description = nullptr;
    }
    if (r.extended_device) {
      if (r.driver->release_description(r.loader, r.extended_device) != MFX_ERR_NONE) return false;
      r.extended_device = nullptr;
    }
    return true;
  }
  Status OpenDevice(const Device& device) {
    auto& r = *resources_; auto& d = *r.driver;
    r.fd = d.open_node(device.node.c_str());
    if (r.fd < 0) {
      const int open_error = errno;
      return Fail(Status::kUnavailable,
          open_error == EACCES || open_error == EPERM
              ? "qsv_render_node_permission_denied" : "qsv_render_node_open_failed");
    }
    drmDevicePtr actual = nullptr;
    const int identified = d.get_device(r.fd, 0, &actual);
    const bool matches = identified == 0 && SameDevice(device, actual);
    if (actual) d.free_device(&actual);
    if (!matches) return Fail(Status::kUnsupported, "qsv_opened_drm_identity_mismatch");
    r.display = d.display(r.fd);
    if (!r.display) return Fail(Status::kUnavailable, "qsv_va_display_unavailable");
    int major = 0, minor = 0;
    if (d.initialize_va(r.display, &major, &minor) != VA_STATUS_SUCCESS)
      return Fail(Status::kUnavailable, "qsv_va_initialize_failed");
    r.va_initialized = true;
    if (major > 0 && major <= 0xffff && minor >= 0 && minor <= 0xffff) {
      r.va_major = static_cast<uint16_t>(major); r.va_minor = static_cast<uint16_t>(minor);
    }
    r.loader = d.load();
    if (!r.loader) return Fail(Status::kUnavailable, "qsv_dispatcher_unavailable");
    if (!Filter("mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE) ||
        !Filter("mfxImplDescription.VendorID", kIntelVendor) ||
        !Filter("mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_VAAPI))
      return Fail(Status::kUnsupported, "qsv_hardware_vaapi_filter_unsupported");
    bool selected = false;
    for (mfxU32 i = 0; i < kMaximumDevices; ++i) {
      const auto listed = d.enumerate(r.loader, i, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &r.description);
      if (listed == MFX_ERR_NOT_FOUND) break;
      if (listed != MFX_ERR_NONE || !r.description) return Fail(Status::kUnavailable, "qsv_implementation_enumeration_failed");
      const auto& description = *static_cast<const mfxImplDescription*>(r.description);
      const bool hardware = description.Impl == MFX_IMPL_TYPE_HARDWARE &&
          description.VendorID == kIntelVendor && description.AccelerationMode == MFX_ACCEL_MODE_VIA_VAAPI;
      if (hardware && d.enumerate(r.loader, i, MFX_IMPLCAPS_DEVICE_ID_EXTENDED, &r.extended_device) != MFX_ERR_NONE)
        return Fail(Status::kUnsupported, "qsv_extended_device_identity_unsupported");
      const bool correct = hardware && r.extended_device &&
          SameDevice(device, *static_cast<const mfxExtendedDeviceId*>(r.extended_device));
      if (!ReleaseDescriptions()) {
        RetainResources("qsv_description_release_failed"); return Status::kRetirementFailed;
      }
      if (!correct) continue;
      if (d.create_session(r.loader, i, &r.session) != MFX_ERR_NONE || !r.session)
        return Fail(Status::kUnavailable, "qsv_create_session_failed");
      selected = true; break;
    }
    if (!selected) return Fail(Status::kUnavailable, "qsv_matching_hardware_implementation_unavailable");
    if (d.set_handle(r.session, MFX_HANDLE_VA_DISPLAY, r.display) != MFX_ERR_NONE)
      return Fail(Status::kUnsupported, "qsv_set_va_display_failed");
    mfxHDL display = nullptr; mfxIMPL implementation = 0; mfxVersion version{};
    if (d.get_handle(r.session, MFX_HANDLE_VA_DISPLAY, &display) != MFX_ERR_NONE || display != r.display ||
        d.query_impl(r.session, &implementation) != MFX_ERR_NONE ||
        MFX_IMPL_BASETYPE(implementation) == MFX_IMPL_SOFTWARE ||
        (implementation & 0x0f00) != MFX_IMPL_VIA_VAAPI ||
        d.query_version(r.session, &version) != MFX_ERR_NONE || version.Major < 1 ||
        (version.Major == 1 && version.Minor < 6))
      return Fail(Status::kUnsupported, "qsv_selected_runtime_or_display_mismatch");
    r.vpl_major = version.Major; r.vpl_minor = version.Minor;
    MakeParameters();
    if (!Accepted(d.query(r.session, &r.parameters, &r.parameters))) return Fail(Status::kUnsupported, "qsv_query_unsupported");
    size_t required = 0;
    if (!PoolRequirement(&required)) return Fail(Status::kUnsupported, "qsv_surface_pool_requirement_unsupported");
    if (!Accepted(d.initialize(r.session, &r.parameters))) return Fail(Status::kUnsupported, "qsv_initialize_failed");
    r.encoder_initialized = true;
    if (!ReadParameters()) return Fail(Status::kUnsupported, "qsv_active_configuration_mismatch");
    if (!PoolRequirement(&required) || !PrepareStorage(required))
      return Fail(Status::kUnsupported, "qsv_pool_allocation_limit");
    return Status::kOk;
  }
  Status ReopenForRates(const Rates& rates) {
    LinuxEncoderRebuildTimer rebuild(LinuxEncoderMetricBackend::kQsv);
    if (!has_selected_device_) {
      Close(); RecordFailure("qsv_rate_reopen_device_missing");
      return Status::kRetirementFailed;
    }
    const Device device = selected_device_;
    auto driver = resources_->driver;
    Config updated = config_;
    updated.bitrate_bps = rates.bitrate_bps;
    updated.framerate_fps = rates.framerate_fps;
    Close();
    if (last_close_status_ != Status::kOk) return Status::kRetirementFailed;
    config_ = updated;
    if (collector_) collector_->BeginOpen();
    unhealthy_ = false;
    resources_ = std::make_unique<Resources>(std::move(driver));
    // OpenDevice validates this exact render node's PCI identity again and
    // matches the dispatcher implementation. Never enumerate another device
    // as an implicit replacement during a bitrate update.
    const auto status = OpenDevice(device);
    if (status == Status::kOk) {
      Selected(device);
      rebuild.Succeeded();
      return Status::kOk;
    }
    Close();
    // A failed in-place stream rebuild is terminal for this hardware route;
    // do not misreport a successful ordinary software fallback.
    if (last_close_status_ == Status::kOk) RecordFailure("qsv_rate_reopen_failed");
    return Status::kRetirementFailed;
  }
  static bool ValidSelection(const LinuxQsvDeviceSelection& selection) {
    constexpr char prefix[] = "/dev/dri/renderD";
    const auto& path = selection.render_node;
    return selection.vendor_id == kIntelVendor && selection.device_id != 0 &&
        selection.pci_domain <= 0xffff && selection.pci_device <= 31 && selection.pci_function <= 7 &&
        path.size() > sizeof(prefix) - 1 && path.size() <= 4096 &&
        path.compare(0, sizeof(prefix) - 1, prefix) == 0 &&
        std::all_of(path.begin() + sizeof(prefix) - 1, path.end(), [](char c) { return c >= '0' && c <= '9'; });
  }
  void Selected(const Device& device) {
    initialized_ = true; need_idr_ = true;
    selected_device_ = device; has_selected_device_ = true; error_.clear();
    if (collector_) {
      const auto& r = *resources_;
      collector_->Opened({device.node, kIntelVendor, device.device,
          device.domain, device.bus, device.slot, device.function,
          r.vpl_major, r.vpl_minor, r.va_major, r.va_minor});
    }
  }
  bool AnyLocked() const {
    return std::any_of(resources_->surfaces.begin(), resources_->surfaces.end(),
                       [](const OwnedSurface& surface) { return surface.frame.Data.Locked != 0; });
  }
  OwnedSurface* AcquireSurface() {
    auto& r = *resources_;
    for (size_t offset = 0; offset < r.surfaces.size(); ++offset) {
      const size_t index = (r.next_surface + offset) % r.surfaces.size();
      if (r.surfaces[index].frame.Data.Locked) continue;
      r.next_surface = (index + 1) % r.surfaces.size();
      return &r.surfaces[index];
    }
    return nullptr;
  }
  bool PoolRequirement(size_t* required) {
    auto& r = *resources_;
    mfxFrameAllocRequest request{};
    if (r.driver->query_surfaces(r.session, &r.parameters, &request) != MFX_ERR_NONE ||
        !(request.Type & MFX_MEMTYPE_SYSTEM_MEMORY)) return false;
    const auto& actual = request.Info;
    const auto& expected = r.parameters.mfx.FrameInfo;
    if (actual.FourCC != MFX_FOURCC_NV12 || actual.ChromaFormat != MFX_CHROMAFORMAT_YUV420 ||
        actual.Width != expected.Width || actual.Height != expected.Height) return false;
    const size_t count = std::max<size_t>(1, std::max(request.NumFrameMin, request.NumFrameSuggested));
    if (count > kMaximumSurfaces) return false;
    *required = count;
    return true;
  }
  bool PrepareStorage(size_t required) {
    auto& r = *resources_;
    if (!required || required > kMaximumSurfaces || r.pending || AnyLocked()) return false;
    const auto& info = r.parameters.mfx.FrameInfo;
    if (!info.Width || !info.Height || info.Width > 8192 || info.Height > 8192 ||
        info.Width % 16 || info.Height % 16) return false;
    const size_t pitch = (static_cast<size_t>(info.Width) + 31) & ~size_t{31};
    size_t y_size = 0, pixel_size = 0, pool_size = 0, doubled = 0, output_floor = 0, hint = 0;
    if (pitch > std::numeric_limits<mfxU16>::max() || !Multiply(pitch, info.Height, &y_size) ||
        !Add(y_size, y_size / 2, &pixel_size) || !Multiply(pixel_size, required, &pool_size) ||
        pool_size > kMaximumBuffer || !Multiply(pixel_size, 2, &doubled) ||
        !Add(doubled, 1024 * 1024, &output_floor) ||
        !Multiply(r.parameters.mfx.BufferSizeInKB, 1000, &hint) ||
        !Multiply(hint, std::max<mfxU16>(1, r.parameters.mfx.BRCParamMultiplier), &hint)) return false;
    const size_t output_size = std::max(hint, output_floor);
    if (output_size > kMaximumBuffer || output_size > std::numeric_limits<mfxU32>::max()) return false;
    const bool dimensions_match = !r.surfaces.empty() && r.surfaces.front().frame.Info.Width == info.Width &&
        r.surfaces.front().frame.Info.Height == info.Height && r.surfaces.front().frame.Data.Pitch == pitch;
    // Storage changes only at Open/Reset, after all previous surface leases
    // are confirmed idle. Steady-state Encode performs no pool allocation.
    if (!dimensions_match || r.surfaces.size() < required) {
      std::vector<OwnedSurface> replacement(required);
      for (auto& surface : replacement) {
        if (!Allocate(pixel_size, &surface.pixels)) return false;
        surface.frame.Info = info;
        surface.frame.Data.Pitch = static_cast<mfxU16>(pitch);
        surface.frame.Data.Y = surface.pixels.get();
        surface.frame.Data.UV = surface.pixels.get() + y_size;
      }
      if (r.pool_bytes)
        RecordLinuxEncoderPoolReleased(LinuxEncoderMetricBackend::kQsv,
            r.surfaces.size(), r.pool_bytes, r.pool_retained);
      r.surfaces = std::move(replacement);
      r.pool_bytes = pool_size;
      r.pool_retained = false;
      RecordLinuxEncoderPoolAllocated(LinuxEncoderMetricBackend::kQsv,
          r.surfaces.size(), r.pool_bytes);
      r.next_surface = 0;
    } else {
      for (auto& surface : r.surfaces) surface.frame.Info = info;
    }
    if (r.bitstream.MaxLength < output_size) {
      AlignedBytes replacement;
      if (!Allocate(output_size, &replacement)) return false;
      r.output = std::move(replacement);
      r.bitstream.Data = r.output.get();
      r.bitstream.MaxLength = static_cast<mfxU32>(output_size);
    }
    return true;
  }
  void RateFields() {
    auto& info = resources_->parameters.mfx;
    info.BRCParamMultiplier = 1;
    info.TargetKbps = static_cast<mfxU16>((config_.bitrate_bps + 999) / 1000);
    info.MaxKbps = info.TargetKbps;
    info.FrameInfo.FrameRateExtN = config_.framerate_fps; info.FrameInfo.FrameRateExtD = 1;
  }
  void MakeParameters() {
    auto& r = *resources_;
    r.options.Header = {MFX_EXTBUFF_CODING_OPTION, sizeof(r.options)};
    r.options.CAVLC = MFX_CODINGOPTION_ON; r.options.MaxDecFrameBuffering = 1;
    r.options2.Header = {MFX_EXTBUFF_CODING_OPTION2, sizeof(r.options2)};
    r.options2.AdaptiveI = MFX_CODINGOPTION_OFF; r.options2.AdaptiveB = MFX_CODINGOPTION_OFF;
    r.options2.ExtBRC = MFX_CODINGOPTION_OFF; r.options2.BRefType = MFX_B_REF_OFF;
    r.options2.LookAheadDepth = 0; r.options2.RepeatPPS = MFX_CODINGOPTION_ON;
    r.extensions = {&r.options.Header, &r.options2.Header};
    auto& p = r.parameters;
    p.ExtParam = r.extensions.data(); p.NumExtParam = r.extensions.size();
    p.AsyncDepth = 1; p.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    auto& info = p.mfx;
    info.CodecId = MFX_CODEC_AVC; info.CodecProfile = MFX_PROFILE_AVC_BASELINE;
    info.CodecLevel = config_.h264_level; info.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    info.RateControlMethod = MFX_RATECONTROL_CBR; info.GopPicSize = 65535;
    info.GopRefDist = 1; info.GopOptFlag = MFX_GOP_CLOSED; info.NumRefFrame = 1;
    auto& frame = info.FrameInfo;
    frame.FourCC = MFX_FOURCC_NV12; frame.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    frame.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    frame.Width = static_cast<mfxU16>((config_.width + 15) & ~15);
    frame.Height = static_cast<mfxU16>((config_.height + 15) & ~15);
    frame.CropW = static_cast<mfxU16>(config_.width); frame.CropH = static_cast<mfxU16>(config_.height);
    RateFields();
  }
  bool ReadParameters() {
    auto& r = *resources_;
    mfxVideoParam actual{}; mfxExtCodingOption options{}; mfxExtCodingOption2 options2{};
    options.Header = {MFX_EXTBUFF_CODING_OPTION, sizeof(options)};
    options2.Header = {MFX_EXTBUFF_CODING_OPTION2, sizeof(options2)};
    std::array<mfxExtBuffer*, 2> extensions{&options.Header, &options2.Header};
    actual.ExtParam = extensions.data(); actual.NumExtParam = extensions.size();
    if (r.driver->get_parameters(r.session, &actual) != MFX_ERR_NONE) return false;
    const auto& info = actual.mfx; const auto& frame = info.FrameInfo;
    if (actual.AsyncDepth != 1 || actual.IOPattern != MFX_IOPATTERN_IN_SYSTEM_MEMORY ||
        info.CodecId != MFX_CODEC_AVC || info.CodecProfile != MFX_PROFILE_AVC_BASELINE ||
        !info.CodecLevel || info.CodecLevel > config_.h264_level || info.GopRefDist != 1 ||
        info.EncodedOrder || info.RateControlMethod != MFX_RATECONTROL_CBR ||
        info.NumRefFrame > 1 || options.CAVLC != MFX_CODINGOPTION_ON ||
        options2.AdaptiveB != MFX_CODINGOPTION_OFF || options2.LookAheadDepth ||
        options2.ExtBRC != MFX_CODINGOPTION_OFF || options2.BRefType != MFX_B_REF_OFF ||
        frame.FourCC != MFX_FOURCC_NV12 || frame.ChromaFormat != MFX_CHROMAFORMAT_YUV420 ||
        frame.PicStruct != MFX_PICSTRUCT_PROGRESSIVE || frame.CropX || frame.CropY ||
        frame.CropW != config_.width || frame.CropH != config_.height ||
        frame.Width < frame.CropW || frame.Height < frame.CropH ||
        frame.Width > 8192 || frame.Height > 8192 || frame.Width % 16 || frame.Height % 16 ||
        frame.BitDepthLuma > 8 || frame.BitDepthChroma > 8 || frame.Shift ||
        !frame.FrameRateExtD || frame.FrameRateExtN != static_cast<uint64_t>(config_.framerate_fps) * frame.FrameRateExtD ||
        static_cast<uint64_t>(info.TargetKbps) * std::max<mfxU16>(1, info.BRCParamMultiplier) !=
            (config_.bitrate_bps + 999) / 1000) return false;
    const mfxVideoParam validated = actual;
    std::array<mfxU8, 16384> sps{}, pps{};
    mfxExtCodingOptionSPSPPS headers{};
    headers.Header = {MFX_EXTBUFF_CODING_OPTION_SPSPPS, sizeof(headers)};
    headers.SPSBuffer = sps.data(); headers.PPSBuffer = pps.data();
    headers.SPSBufSize = sps.size(); headers.PPSBufSize = pps.size();
    mfxExtBuffer* header = &headers.Header;
    actual = {}; actual.ExtParam = &header; actual.NumExtParam = 1;
    if (r.driver->get_parameters(r.session, &actual) != MFX_ERR_NONE ||
        headers.SPSBufSize > sps.size() || headers.PPSBufSize > pps.size() ||
        !SaveHeader(sps.data(), headers.SPSBufSize, 7, &sps_) ||
        !SaveHeader(pps.data(), headers.PPSBufSize, 8, &pps_)) return false;
    r.parameters = validated; r.options = options; r.options2 = options2;
    r.parameters.ExtParam = r.extensions.data(); r.parameters.NumExtParam = r.extensions.size();
    return true;
  }
  int ordinal_;
  const std::optional<LinuxQsvDeviceSelection> expected_;
  const std::shared_ptr<internal::LinuxQsvDeviceCollector> collector_;
  std::shared_ptr<internal::LinuxQsvDriver> injected_;
  std::unique_ptr<Resources> resources_;
  Config config_;
  Device selected_device_;
  bool has_selected_device_ = false;
  bool initialized_ = false, unhealthy_ = false, need_idr_ = true;
  bool retirement_unconfirmed_ = false;
  Status last_close_status_ = Status::kOk;
  std::string error_;
  std::vector<uint8_t> sps_, pps_;
};
}  // namespace
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackend(int device_ordinal) {
  return std::make_unique<LinuxQsvBackend>(device_ordinal);
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackend(const LinuxQsvDeviceSelection& selection) {
  return std::make_unique<LinuxQsvBackend>(-1, nullptr, selection);
}
namespace internal {
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForProbe(
    int ordinal, std::shared_ptr<LinuxQsvDeviceCollector> collector) {
  return std::make_unique<LinuxQsvBackend>(ordinal, nullptr, std::nullopt, std::move(collector));
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForTesting(
    std::shared_ptr<LinuxQsvDriver> driver, int device_ordinal) {
  return std::make_unique<LinuxQsvBackend>(device_ordinal, std::move(driver));
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForTesting(
    std::shared_ptr<LinuxQsvDriver> driver, int ordinal,
    std::shared_ptr<LinuxQsvDeviceCollector> collector) {
  return std::make_unique<LinuxQsvBackend>(ordinal, std::move(driver), std::nullopt, std::move(collector));
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForTesting(
    std::shared_ptr<LinuxQsvDriver> driver, const LinuxQsvDeviceSelection& selection) {
  return std::make_unique<LinuxQsvBackend>(-1, std::move(driver), selection);
}
}  // namespace internal
}  // namespace webrtc::external_encoder
