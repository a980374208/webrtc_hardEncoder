#include "modules/video_coding/codecs/external/vaapi_video_encoder_backend_linux.h"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>
#include <unistd.h>
#include "modules/video_coding/codecs/external/linux_vaapi_driver.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"
namespace webrtc::external_encoder {
namespace {
Status Convert(CohavoraVaapiStatus status) {
  switch (status) {
    case COHAVORA_VAAPI_OK: return Status::kOk;
    case COHAVORA_VAAPI_UNAVAILABLE: return Status::kUnavailable;
    case COHAVORA_VAAPI_UNSUPPORTED: return Status::kUnsupported;
    case COHAVORA_VAAPI_INVALID_ARGUMENT: return Status::kInvalidArgument;
    case COHAVORA_VAAPI_NO_OUTPUT: return Status::kNoOutput;
    case COHAVORA_VAAPI_RETIREMENT_FAILED: return Status::kRetirementFailed;
    default: return Status::kError;
  }
}
bool RatesValid(const Rates& value) {
  return value.bitrate_bps && value.framerate_fps && value.framerate_fps <= 240;
}
bool LevelValid(uint8_t level) {
  constexpr std::array<uint8_t, 19> levels =
      {10,11,12,13,20,21,22,30,31,32,40,41,42,50,51,52,60,61,62};
  return std::find(levels.begin(), levels.end(), level) != levels.end();
}
struct Resources {
  explicit Resources(std::shared_ptr<internal::LinuxVaapiDriver> value)
      : driver(std::move(value)) {}
  std::shared_ptr<internal::LinuxVaapiDriver> driver;
  void* session = nullptr;
  int pin_fd = -1;
};
class LinuxVaapiBackend final : public VideoEncoderBackend {
 public:
  LinuxVaapiBackend(std::string requested, std::string runtime,
      std::shared_ptr<internal::LinuxVaapiDriver> driver = {},
      std::vector<internal::LinuxVaapiDevice> devices = {},
      std::optional<LinuxVaapiDeviceSelection> expected = {},
      std::shared_ptr<internal::LinuxVaapiDeviceCollector> collector = {})
      : requested_device_(std::move(requested)), runtime_path_(std::move(runtime)),
        expected_(std::move(expected)), collector_(std::move(collector)),
        injected_driver_(std::move(driver)), injected_devices_(std::move(devices)) {
    RecordLinuxEncoderBackendCreated(LinuxEncoderMetricBackend::kVaapi);
  }
  ~LinuxVaapiBackend() override {
    Close();
    if (retirement_unconfirmed_) (void)resources_.release();
  }
  Info GetInfo() const override { return {"VAAPI-Linux", true}; }
  Status Open(const Config& config) override {
    RecordLinuxEncoderBackendOpenAttempt(LinuxEncoderMetricBackend::kVaapi);
    Close();
    if (collector_) collector_->BeginOpen();
    if (close_status_ != Status::kOk ||
        CheckLinuxVaapiRetirement() != Status::kOk)
      return Fail(Status::kRetirementFailed, "vaapi_retirement_unconfirmed");
    if (config.codec != Codec::kH264 || !LevelValid(config.h264_level))
      return Fail(Status::kUnsupported, "vaapi_h264_baseline_only");
    if (config.width <= 0 || config.height <= 0 || config.width > 8192 ||
        config.height > 8192 || (config.width & 1) || (config.height & 1) ||
        !RatesValid({config.bitrate_bps, config.framerate_fps}))
      return Fail(Status::kInvalidArgument, "vaapi_invalid_configuration");
    if ((expected_ && (!internal::ValidLinuxVaapiSelection(*expected_) ||
          (!requested_device_.empty() && requested_device_ != expected_->render_node))) ||
        requested_device_.find('\0') != std::string::npos)
      return Fail(Status::kInvalidArgument, "vaapi_invalid_device_path");
    config_ = config;
    Status load = Status::kOk;
    auto driver = injected_driver_;
    if (!driver) driver = internal::LoadLinuxVaapiDriver(runtime_path_, &load, &error_);
    if (!driver) return load;
    if (!driver->HasRequiredFunctions())
      return Fail(Status::kUnavailable, "vaapi_runtime_function_missing");
    const auto devices = injected_driver_ ? injected_devices_ :
        internal::EnumerateLinuxVaapiDevices();
    Status status = Status::kUnavailable;
    bool attempted = false;
    std::string last_device_error;
    for (const auto& device : devices) {
      if (!requested_device_.empty() && requested_device_ != device.path) continue;
      const auto selection = internal::LinuxVaapiSelection(device);
      if (!selection || (expected_ && !internal::SameLinuxVaapiSelection(*selection, *expected_)))
        continue;
      attempted = true;
      resources_ = std::make_unique<Resources>(driver);
      std::string native_path = device.path;
      if (!injected_driver_) {
        status = internal::PinLinuxVaapiDevice(device, &resources_->pin_fd, &error_);
        if (status == Status::kRetirementFailed) {
          MarkRetirement("vaapi_pin_retirement_failed"); return status;
        }
        if (status != Status::kOk) {
          last_device_error=error_;
          resources_.reset();
          continue;
        }
        native_path = "/proc/self/fd/" + std::to_string(resources_->pin_fd);
      }
      last_device_error.clear();
      const CohavoraVaapiConfig native{sizeof(CohavoraVaapiConfig),
          static_cast<uint32_t>(config.width), static_cast<uint32_t>(config.height),
          config.bitrate_bps, config.framerate_fps, config.h264_level,
          native_path.c_str()};
      status = Convert(driver->api.open(&native, &resources_->session));
      if (status == Status::kOk && !resources_->session) status = Status::kError;
      if (status == Status::kOk) {
        const size_t size = static_cast<size_t>(config.width) * config.height * 3 + 65536;
        storage_.resize(std::min(size, size_t{32} * 1024 * 1024));
        opened_ = true; unhealthy_ = false; next_key_ = true; error_.clear();
        if (collector_) collector_->Opened(*selection);
        RecordLinuxEncoderBackendOpenSucceeded(LinuxEncoderMetricBackend::kVaapi);
        return Status::kOk;
      }
      if (status == Status::kRetirementFailed) {
        MarkRetirement("vaapi_open_retirement_failed");
        return status;
      }
      Close();
      if (close_status_ != Status::kOk) return close_status_;
    }
    if(!last_device_error.empty())return Fail(status,last_device_error.c_str());
    return Fail(status, attempted ? "vaapi_device_not_supported" : "vaapi_device_unavailable");
  }
  Status SetRates(const Rates& rates) override {
    if (CheckLinuxVaapiRetirement() != Status::kOk)
      return Fail(Status::kRetirementFailed, "vaapi_retirement_unconfirmed");
    if (!opened_ || unhealthy_ || !RatesValid(rates))
      return Fail(Status::kInvalidArgument, "vaapi_invalid_rates");
    if (rates.bitrate_bps == config_.bitrate_bps &&
        rates.framerate_fps == config_.framerate_fps) return Status::kOk;
    const auto status = Convert(resources_->driver->api.set_rates(
        resources_->session, rates.bitrate_bps, rates.framerate_fps));
    if (status != Status::kOk) {
      unhealthy_ = true;
      if (status == Status::kRetirementFailed) MarkRetirement("vaapi_rates_retirement_failed");
      return Fail(status, "vaapi_rate_rebuild_failed");
    }
    config_.bitrate_bps = rates.bitrate_bps;
    config_.framerate_fps = rates.framerate_fps;
    next_key_ = true;
    return Status::kOk;
  }
  Status Encode(const Frame& frame, bool force_key, Packet* packet) override {
    if (packet) *packet = {};
    if (CheckLinuxVaapiRetirement() != Status::kOk)
      return Fail(Status::kRetirementFailed, "vaapi_retirement_unconfirmed");
    if (!packet || !opened_ || unhealthy_ || !frame.y || !frame.uv ||
        frame.width != config_.width || frame.height != config_.height ||
        frame.stride_y < frame.width || frame.stride_uv < frame.width)
      return Fail(Status::kInvalidArgument, "vaapi_invalid_frame");
    const CohavoraVaapiFrame native{sizeof(CohavoraVaapiFrame), frame.y, frame.uv,
        frame.stride_y, frame.stride_uv, static_cast<uint32_t>(frame.width),
        static_cast<uint32_t>(frame.height), frame.id};
    CohavoraVaapiPacket output{sizeof(CohavoraVaapiPacket), storage_.data(),
        storage_.size(), 0, 0, 0, -1};
    const auto status = Convert(resources_->driver->api.encode(resources_->session,
        &native, force_key || next_key_, &output));
    if (status != Status::kOk) {
      // Delayed output is incompatible with the synchronous frame contract.
      // Never accept a later frame or retry/flush to relabel an old packet.
      unhealthy_ = true;
      if (status == Status::kRetirementFailed) MarkRetirement("vaapi_encode_retirement_failed");
      return Fail(status == Status::kNoOutput ? Status::kUnsupported : status,
                  "vaapi_synchronous_output_unavailable");
    }
    if (!output.size || output.size > output.capacity ||
        output.capacity != storage_.size() || output.data != storage_.data() ||
        output.frame_id != frame.id || output.key_frame > 1 ||
        ((force_key || next_key_) && !output.key_frame)) {
      unhealthy_ = true;
      return Fail(Status::kError, "vaapi_output_identity_invalid");
    }
    packet->bytes.assign(storage_.begin(), storage_.begin() + output.size);
    packet->frame_id = output.frame_id;
    packet->key_frame = output.key_frame != 0;
    packet->qp = output.qp;
    next_key_ = false;
    return Status::kOk;
  }
  void Close() override {
    opened_ = false;
    if (retirement_unconfirmed_) return;
    if (resources_ && resources_->session) {
      const auto status = Convert(resources_->driver->api.close(&resources_->session));
      if (status != Status::kOk || resources_->session) {
        MarkRetirement("vaapi_close_failed"); return;
      }
    }
    // Retain the pin alongside the runtime until native retirement is certain.
    if (resources_ && resources_->pin_fd >= 0) {
      if (::close(resources_->pin_fd) != 0) {
        MarkRetirement("vaapi_pin_retirement_failed"); return;
      }
      resources_->pin_fd = -1;
    }
    resources_.reset();
    close_status_ = Status::kOk;
    if (collector_) collector_->Closed();
  }
  Status LastCloseStatus() const override { return close_status_; }
  std::string_view LastError() const override { return error_; }
 private:
  Status Fail(Status value, const char* text) { error_ = text; return value; }
  void MarkRetirement(const char* text) {
    close_status_ = Status::kRetirementFailed;
    retirement_unconfirmed_ = true; error_ = text;
    if (collector_) collector_->FailedRetirement();
    RecordLinuxVaapiRetirement(text);
  }
  const std::string requested_device_, runtime_path_;
  const std::optional<LinuxVaapiDeviceSelection> expected_;
  const std::shared_ptr<internal::LinuxVaapiDeviceCollector> collector_;
  std::shared_ptr<internal::LinuxVaapiDriver> injected_driver_;
  std::vector<internal::LinuxVaapiDevice> injected_devices_;
  std::unique_ptr<Resources> resources_;
  Config config_;
  std::vector<uint8_t> storage_;
  std::string error_;
  Status close_status_ = Status::kOk;
  bool opened_ = false, unhealthy_ = false, next_key_ = true;
  bool retirement_unconfirmed_ = false;
};
}  // namespace
std::unique_ptr<VideoEncoderBackend> CreateLinuxVaapiBackend(
    std::string device_path, std::string runtime_path,
    std::shared_ptr<internal::LinuxVaapiDeviceCollector> collector) {
  return std::make_unique<LinuxVaapiBackend>(std::move(device_path), std::move(runtime_path),
      nullptr, std::vector<internal::LinuxVaapiDevice>{}, std::nullopt, std::move(collector));
}
std::unique_ptr<VideoEncoderBackend> CreateLinuxVaapiBackend(
    const LinuxVaapiDeviceSelection& selection, std::string runtime_path,
    std::shared_ptr<internal::LinuxVaapiDeviceCollector> collector) {
  return std::make_unique<LinuxVaapiBackend>(selection.render_node, std::move(runtime_path),
      nullptr, std::vector<internal::LinuxVaapiDevice>{}, selection, std::move(collector));
}
namespace internal {
std::unique_ptr<VideoEncoderBackend> CreateLinuxVaapiBackendForTesting(
    std::shared_ptr<LinuxVaapiDriver> driver, std::vector<LinuxVaapiDevice> devices,
    std::string requested_device,
    std::optional<LinuxVaapiDeviceSelection> expected,
    std::shared_ptr<LinuxVaapiDeviceCollector> collector) {
  return std::make_unique<LinuxVaapiBackend>(std::move(requested_device), "",
      std::move(driver), std::move(devices), std::move(expected), std::move(collector));
}
}  // namespace internal
}  // namespace webrtc::external_encoder
