// Standalone fake C ABI driver tests; no GPU or runtime library is used.
#include "modules/video_coding/codecs/external/linux_vaapi_driver.h"
#include "modules/video_coding/codecs/external/vaapi_video_encoder_backend_linux.h"
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
namespace {
using namespace webrtc::external_encoder;
using namespace webrtc::external_encoder::internal;
struct Fake {
  int open = 0, close = 0, encode = 0, rates = 0, live = 0;
  bool close_fail = false, partial_fail = false, wrong_id = false;
  bool no_output = false, rates_fail = false;
  bool last_key = false;
  std::string device;
} state;
void Check(bool value) { if (!value) std::abort(); }
CohavoraVaapiStatus Open(const CohavoraVaapiConfig* config, void** handle) {
  ++state.open; ++state.live;
  state.device = config->device_path; *handle = new int(7);
  return state.partial_fail ? COHAVORA_VAAPI_UNSUPPORTED : COHAVORA_VAAPI_OK;
}
CohavoraVaapiStatus Close(void** handle) {
  ++state.close;
  if (state.close_fail) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  delete static_cast<int*>(*handle); *handle = nullptr; --state.live;
  return COHAVORA_VAAPI_OK;
}
CohavoraVaapiStatus Rates(void*, uint32_t bitrate, uint32_t fps) {
  ++state.rates; Check(bitrate == 128000 && fps == 20);
  return state.rates_fail ? COHAVORA_VAAPI_RETIREMENT_FAILED : COHAVORA_VAAPI_OK;
}
CohavoraVaapiStatus Encode(void*, const CohavoraVaapiFrame* frame, uint32_t key,
                         CohavoraVaapiPacket* packet) {
  ++state.encode; state.last_key = key;
  Check(frame->y[0] == 13 && frame->uv[0] == 17);
  if (state.no_output) return COHAVORA_VAAPI_NO_OUTPUT;
  static constexpr uint8_t bytes[]{0,0,0,1,0x65,0x80,0x11};
  Check(packet->capacity >= sizeof(bytes));
  std::memcpy(packet->data, bytes, sizeof(bytes)); packet->size = sizeof(bytes);
  packet->frame_id = frame->frame_id + state.wrong_id;
  packet->key_frame = key; packet->qp = 23;
  return COHAVORA_VAAPI_OK;
}
CohavoraVaapiStatus Metrics(CohavoraVaapiMetrics* out) {
  if(!out || out->struct_size<sizeof(*out))return COHAVORA_VAAPI_INVALID_ARGUMENT;
  *out={};out->struct_size=sizeof(*out);out->schema_version=1;out->scope=1;
  return COHAVORA_VAAPI_OK;
}
std::shared_ptr<LinuxVaapiDriver> Driver() {
  auto d = std::make_shared<LinuxVaapiDriver>();
  d->api = {sizeof(CohavoraVaapiApi), COHAVORA_VAAPI_ABI_VERSION,
            Open, Rates, Encode, Close, Metrics};
  return d;
}
std::vector<LinuxVaapiDevice> Devices() {
  return {{"/dev/dri/renderD132", "0000:04:00.0", 0x1002, 0x7340},
          {"/dev/dri/renderD140", "0000:08:00.0", 0x8086, 0x46a6}};
}
Config Configuration() { Config c; c.width=160; c.height=128;
  c.bitrate_bps=64000; c.framerate_fps=15; return c; }
struct Input {
  std::vector<uint8_t> y = std::vector<uint8_t>(160*128,13);
  std::vector<uint8_t> uv = std::vector<uint8_t>(160*64,17);
  Frame frame() { return {y.data(),uv.data(),160,160,160,128,19}; }
};
void Happy() {
  auto d=Driver(); auto b=CreateLinuxVaapiBackendForTesting(d, Devices());
  Check(b->Open(Configuration())==Status::kOk && state.device=="/dev/dri/renderD132");
  Input input; Packet first, second;
  Check(b->Encode(input.frame(),false,&first)==Status::kOk && first.key_frame);
  const auto first_bytes=first.bytes;
  Check(b->Encode(input.frame(),false,&second)==Status::kOk && !second.key_frame);
  Check(first.bytes==first_bytes && first.frame_id==19 && first.qp==23);
  Check(b->SetRates({128000,20})==Status::kOk && state.rates==1);
  Check(b->Encode(input.frame(),false,&second)==Status::kOk && second.key_frame);
  b->Close(); b->Close();
  Check(state.close==1 && state.live==0 && b->LastCloseStatus()==Status::kOk);
}
void ExplicitDevice() { auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"/dev/dri/renderD140");
  Check(b->Open(Configuration())==Status::kOk && state.open==1 && state.device=="/dev/dri/renderD140"); }
void MissingDevice() { auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"/dev/dri/renderD999");
  Check(b->Open(Configuration())==Status::kUnavailable && state.open==0); }
void NoDevices() { auto b=CreateLinuxVaapiBackendForTesting(Driver(),{});
  Check(b->Open(Configuration())==Status::kUnavailable && state.open==0); }
void MissingFunction() { auto d=Driver(); d->api.close=nullptr;
  auto b=CreateLinuxVaapiBackendForTesting(d,Devices());
  Check(b->Open(Configuration())==Status::kUnavailable && state.open==0); }
void InvalidConfig() { auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices());
  auto c=Configuration(); c.width=159; Check(b->Open(c)==Status::kInvalidArgument);
  c=Configuration(); c.codec=Codec::kH265; Check(b->Open(c)==Status::kUnsupported && state.open==0); }
void PartialOpenCleanup() { state.partial_fail=true;
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices());
  Check(b->Open(Configuration())==Status::kUnsupported && state.open==2 &&
        state.close==2 && state.live==0 && CheckLinuxVaapiRetirement()==Status::kOk); }
void DelayedOutput() { auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices());
  Check(b->Open(Configuration())==Status::kOk); state.no_output=true;
  Input input; Packet packet;
  Check(b->Encode(input.frame(),false,&packet)==Status::kUnsupported && packet.bytes.empty());
  Check(b->Encode(input.frame(),false,&packet)==Status::kInvalidArgument && state.encode==1);
  b->Close(); Check(state.live==0 && CheckLinuxVaapiRetirement()==Status::kOk); }
void WrongIdentity() { auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices());
  Check(b->Open(Configuration())==Status::kOk); state.wrong_id=true; Input input; Packet packet;
  Check(b->Encode(input.frame(),false,&packet)==Status::kError && packet.bytes.empty()); }
void CloseFailureRetains() { auto d=Driver(); std::weak_ptr<LinuxVaapiDriver> weak=d;
  auto b=CreateLinuxVaapiBackendForTesting(d,Devices()); d.reset();
  Check(b->Open(Configuration())==Status::kOk); state.close_fail=true;
  b->Close(); Check(b->LastCloseStatus()==Status::kRetirementFailed && state.live==1);
  Check(CheckLinuxVaapiRetirement()==Status::kRetirementFailed &&
        CheckLinuxBackendRetirement()==Status::kRetirementFailed);
  b.reset(); Check(state.close==1 && state.live==1 && !weak.expired()); }
void RatesFailurePoison() { auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices());
  Check(b->Open(Configuration())==Status::kOk); state.rates_fail=true;
  Check(b->SetRates({128000,20})==Status::kRetirementFailed);
  Check(CheckLinuxBackendRetirement()==Status::kRetirementFailed); }
void PartialOpenRetirement() { state.partial_fail=true; state.close_fail=true;
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices());
  Check(b->Open(Configuration())==Status::kRetirementFailed && state.open==1 && state.close==1);
  Check(CheckLinuxBackendRetirement()==Status::kRetirementFailed); }
void OwnedSelectionAfterClose() {
  auto collector=std::make_shared<LinuxVaapiDeviceCollector>();
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"",std::nullopt,collector);
  Check(!collector->RetiredSelection());
  Check(b->Open(Configuration())==Status::kOk && !collector->RetiredSelection());
  b->Close(); auto selected=collector->RetiredSelection();
  Check(selected && selected->render_node=="/dev/dri/renderD132" &&
        selected->vendor_id==0x1002 && selected->device_id==0x7340 &&
        selected->pci_domain==0 && selected->pci_bus==4 &&
        selected->pci_device==0 && selected->pci_function==0);
  // A new in-flight Open must never expose the previous successful probe.
  Check(b->Open(Configuration())==Status::kOk && !collector->RetiredSelection());
  b->Close(); Check(collector->RetiredSelection().has_value());
}
void TypedSelectionSameDevice() {
  const auto selected=LinuxVaapiSelection(Devices()[1]); Check(selected.has_value());
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"",selected);
  Check(b->Open(Configuration())==Status::kOk && state.open==1 &&
        state.device=="/dev/dri/renderD140");
}
void TypedDeviceIdDrift() {
  auto selected=LinuxVaapiSelection(Devices()[0]); Check(selected.has_value()); ++selected->device_id;
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"",selected);
  Check(b->Open(Configuration())==Status::kUnavailable && state.open==0);
}
void TypedPciDriftNoFallback() {
  auto selected=LinuxVaapiSelection(Devices()[0]); Check(selected.has_value()); ++selected->pci_bus;
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"",selected);
  Check(b->Open(Configuration())==Status::kUnavailable && state.open==0);
}
void TypedPathConflict() {
  auto selected=LinuxVaapiSelection(Devices()[0]); Check(selected.has_value());
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"/dev/dri/renderD140",selected);
  Check(b->Open(Configuration())==Status::kInvalidArgument && state.open==0);
}
void TypedMissingNoFallback() {
  auto selected=LinuxVaapiSelection(Devices()[0]); Check(selected.has_value());
  auto devices=Devices();devices.erase(devices.begin());
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),devices,"",selected);
  Check(b->Open(Configuration())==Status::kUnavailable && state.open==0);
}
void FailedRetirementNoSelection() {
  auto collector=std::make_shared<LinuxVaapiDeviceCollector>();
  auto b=CreateLinuxVaapiBackendForTesting(Driver(),Devices(),"",std::nullopt,collector);
  Check(b->Open(Configuration())==Status::kOk);state.close_fail=true;b->Close();
  Check(b->LastCloseStatus()==Status::kRetirementFailed && !collector->RetiredSelection());
}
void InvalidDeviceIdentities() {
  auto device=Devices()[0];device.pci="garbage";Check(!LinuxVaapiSelection(device));
  device=Devices()[0];device.device=0;Check(!LinuxVaapiSelection(device));
  device=Devices()[0];device.path="/dev/dri/renderD128/../card0";
  Check(!LinuxVaapiSelection(device));
  auto value=LinuxVaapiSelection(Devices()[0]);Check(value.has_value());value->pci_domain=0x10000;
  Check(!ValidLinuxVaapiSelection(*value));
}
void MissingRuntime() { Status status=Status::kOk; std::string error;
  auto driver=LoadLinuxVaapiDriver("/cohavora/not-present/libcohavora_vaapi_runtime.so.1",&status,&error);
  Check(!driver && status==Status::kUnavailable && error=="vaapi_runtime_unavailable"); }
void InvalidRuntimePath() { Status status=Status::kOk; std::string error;
  Check(!LoadLinuxVaapiDriver("./untrusted.so",&status,&error) && status==Status::kInvalidArgument); }
}  // namespace
int main() {
  const std::pair<const char*,void(*)()> cases[]{
    {"owned_packet_rates_idr_close",Happy},{"explicit_device",ExplicitDevice},
    {"missing_device",MissingDevice},{"no_devices",NoDevices},
    {"missing_function",MissingFunction},{"invalid_config",InvalidConfig},
    {"partial_open_cleanup",PartialOpenCleanup},{"delayed_output_rejected",DelayedOutput},
    {"identity_mismatch_rejected",WrongIdentity},{"close_failure_retained",CloseFailureRetains},
    {"rates_failure_global_poison",RatesFailurePoison},
    {"partial_open_retirement",PartialOpenRetirement},{"missing_runtime",MissingRuntime},
    {"relative_runtime_rejected",InvalidRuntimePath},
    {"owned_selection_after_close",OwnedSelectionAfterClose},
    {"typed_selection_same_device",TypedSelectionSameDevice},
    {"typed_device_id_drift",TypedDeviceIdDrift},
    {"typed_pci_drift_no_fallback",TypedPciDriftNoFallback},
    {"typed_path_conflict",TypedPathConflict},
    {"typed_missing_no_fallback",TypedMissingNoFallback},
    {"failed_retirement_no_selection",FailedRetirementNoSelection},
    {"invalid_device_identities",InvalidDeviceIdentities}};
  for (const auto& item : cases) {
    const auto child=fork(); if(child<0)return 1;
    if(child==0){ item.second(); std::printf("%s PASS\n",item.first); std::fflush(stdout); _exit(0); }
    int status=0; if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
  }
  std::printf("linux_vaapi_fake_tests PASS checks=%zu hardware=NOT_RUN\n",sizeof(cases)/sizeof(cases[0]));
}
