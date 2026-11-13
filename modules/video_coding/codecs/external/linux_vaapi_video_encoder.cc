#include "modules/video_coding/codecs/external/linux_vaapi_video_encoder.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/vaapi_video_encoder_backend_linux.h"
namespace webrtc {
namespace {
std::unique_ptr<external_encoder::VideoEncoderBackend> BoundedVaapi(
    const LinuxVaapiEncoderOptions& options,
    std::shared_ptr<external_encoder::internal::LinuxVaapiDeviceCollector> collector = {},
    const external_encoder::LinuxVaapiDeviceSelection* selection = nullptr) {
  using namespace external_encoder;
  auto inner = selection ? CreateLinuxVaapiBackend(*selection, options.runtime_path, collector) :
      CreateLinuxVaapiBackend(options.device_path, options.runtime_path, collector);
  return CreateLinuxBoundedBackendWithRetirementHooks(
      std::move(inner),
      {"VAAPI-Linux", true}, {CheckLinuxVaapiRetirement, RecordLinuxVaapiRetirement});
}
}  // namespace
std::unique_ptr<VideoEncoder> CreateLinuxVaapiVideoEncoder(
    const Environment&, const SdpVideoFormat& format,
    const LinuxVaapiEncoderOptions& options) {
  if (!IsBackendH264FormatSupported(format) ||
      external_encoder::CheckLinuxBackendRetirement() != external_encoder::Status::kOk ||
      external_encoder::CheckLinuxVaapiRetirement() != external_encoder::Status::kOk)
    return nullptr;
  return CreateBackendVideoEncoder(format, BoundedVaapi(options));
}
std::unique_ptr<VideoEncoder> CreateLinuxVaapiVideoEncoder(
    const Environment&, const SdpVideoFormat& format,
    const external_encoder::LinuxVaapiDeviceSelection& selection,
    const LinuxVaapiEncoderOptions& options) {
  if (!IsBackendH264FormatSupported(format) ||
      (!options.device_path.empty() && options.device_path != selection.render_node) ||
      external_encoder::CheckLinuxBackendRetirement() != external_encoder::Status::kOk ||
      external_encoder::CheckLinuxVaapiRetirement() != external_encoder::Status::kOk)
    return nullptr;
  return CreateBackendVideoEncoder(format, BoundedVaapi(options, {}, &selection));
}
external_encoder::Status ProbeLinuxVaapiVideoEncoderAvailability(
    const SdpVideoFormat& format, const LinuxVaapiEncoderOptions& options,
    std::string* reason) {
  return ProbeLinuxVaapiVideoEncoderAvailability(format, options, reason, nullptr);
}
external_encoder::Status ProbeLinuxVaapiVideoEncoderAvailability(
    const SdpVideoFormat& format, const LinuxVaapiEncoderOptions& options,
    std::string* reason, external_encoder::LinuxVaapiDeviceSelection* resolved) {
  using namespace external_encoder;
  if (resolved) *resolved = {};
  if (!IsBackendH264FormatSupported(format)) {
    if (reason) *reason = "unsupported_linux_vaapi_format";
    return Status::kUnsupported;
  }
  if (CheckLinuxBackendRetirement() != Status::kOk ||
      CheckLinuxVaapiRetirement() != Status::kOk) {
    if (reason) *reason = "vaapi_prior_retirement_failure";
    return Status::kRetirementFailed;
  }
  const auto profile = ParseSdpForH264ProfileLevelId(format.parameters);
  if (!profile || profile->level == H264Level::kLevel1_b) {
    if (reason) *reason = "unsupported_linux_vaapi_level";
    return Status::kUnsupported;
  }
  auto collector = std::make_shared<external_encoder::internal::LinuxVaapiDeviceCollector>();
  auto backend = BoundedVaapi(options, collector);
  Config config;
  config.width = 160; config.height = 128;
  config.bitrate_bps = 64000; config.framerate_fps = 15;
  config.h264_level = static_cast<uint8_t>(profile->level);
  auto status = backend->Open(config);
  backend->Close();
  if (backend->LastCloseStatus() != Status::kOk) status = Status::kRetirementFailed;
  if (reason) reason->assign(backend->LastError());
  backend.reset(); // Include the actual inner destructor before approving probe.
  if (CheckLinuxBackendRetirement() != Status::kOk ||
      CheckLinuxVaapiRetirement() != Status::kOk) status = Status::kRetirementFailed;
  if (status == Status::kOk) {
    auto selected = collector->RetiredSelection();
    if (!selected) {
      if (reason) *reason = "linux_vaapi_resolved_device_unconfirmed";
      return Status::kError;
    }
    if (resolved) *resolved = std::move(*selected);
  }
  return status;
}
}  // namespace webrtc
