#include "modules/video_coding/codecs/external/linux_vaapi_driver.h"
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <optional>
#include <fstream>
#include <mutex>
#include <utility>
#include "modules/video_coding/codecs/external/vaapi_video_encoder_backend_linux.h"
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
namespace webrtc::external_encoder {
namespace {
struct Retirement { std::mutex mutex; bool failed = false; std::string reason; };
Retirement& RetirementState() { static auto* state = new Retirement; return *state; }
}  // namespace
Status CheckLinuxVaapiRetirement() {
  auto& state = RetirementState();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.failed ? Status::kRetirementFailed : Status::kOk;
}
void RecordLinuxVaapiRetirement(std::string_view reason) {
  RecordLinuxBackendRetirementFailure(reason);
  auto& state = RetirementState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.failed) { state.failed = true; state.reason.assign(reason); }
}
namespace internal {
LinuxVaapiDriver::~LinuxVaapiDriver() { if (library) dlclose(library); }
bool LinuxVaapiDriver::HasRequiredFunctions() const {
  return api.abi_version == COHAVORA_VAAPI_ABI_VERSION &&
      api.struct_size >= sizeof(CohavoraVaapiApi) && api.open && api.set_rates &&
      api.encode && api.close && api.get_metrics;
}
std::shared_ptr<LinuxVaapiDriver> LoadLinuxVaapiDriver(
    const std::string& runtime_path, Status* status, std::string* reason) {
  auto fail = [&](Status value, const char* text) {
    if (status) *status = value;
    if (reason) *reason = text;
    return std::shared_ptr<LinuxVaapiDriver>{};
  };
  if (!runtime_path.empty() && (runtime_path[0] != '/' ||
      runtime_path.find('\0') != std::string::npos))
    return fail(Status::kInvalidArgument, "vaapi_runtime_path_invalid");
  auto driver = std::make_shared<LinuxVaapiDriver>();
  driver->library = dlopen(runtime_path.empty() ? COHAVORA_VAAPI_RUNTIME_SONAME :
      runtime_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!driver->library) return fail(Status::kUnavailable, "vaapi_runtime_unavailable");
  auto get_api = reinterpret_cast<CohavoraVaapiGetApi>(
      dlsym(driver->library, "cohavora_vaapi_get_api"));
  driver->api.struct_size = sizeof(driver->api);
  if (!get_api || get_api(COHAVORA_VAAPI_ABI_VERSION, &driver->api) !=
      COHAVORA_VAAPI_OK || !driver->HasRequiredFunctions())
    return fail(Status::kUnavailable, "vaapi_runtime_abi_mismatch");
  if (status) *status = Status::kOk;
  if (reason) reason->clear();
  return driver;
}
namespace {
std::optional<LinuxVaapiDevice> ReadIdentity(const std::string& sysfs,
                                           std::string path) {
  auto read_id = [&](const char* name) -> std::optional<uint32_t> {
    std::ifstream input(sysfs + name);
    std::string text;
    if (!(input >> text) || text.empty() || text.size() > 10) return std::nullopt;
    char* end = nullptr;
    const auto value = std::strtoul(text.c_str(), &end, 0);
    if (!end || *end || value > 0xffff) return std::nullopt;
    return static_cast<uint32_t>(value);
  };
  const auto vendor = read_id("/vendor");
  const auto device = read_id("/device");
  if (!vendor || !device || (*vendor != 0x1002 && *vendor != 0x8086))
    return std::nullopt;
  std::array<char,4096> target{};
  const auto count = readlink(sysfs.c_str(), target.data(), target.size() - 1);
  if (count <= 0 || static_cast<size_t>(count) >= target.size() - 1)
    return std::nullopt;
  target[static_cast<size_t>(count)] = 0;
  const char* base = std::strrchr(target.data(), '/');
  if (!base || !base[1]) return std::nullopt;
  LinuxVaapiDevice result{std::move(path), base + 1, *vendor, *device};
  if (!LinuxVaapiSelection(result)) return std::nullopt;
  return result;
}
}  // namespace
bool ValidLinuxVaapiSelection(const LinuxVaapiDeviceSelection& value) {
  constexpr char prefix[] = "/dev/dri/renderD";
  if (value.render_node.size() <= sizeof(prefix) - 1 ||
      value.render_node.compare(0, sizeof(prefix) - 1, prefix) != 0 ||
      value.render_node.size() >= 256 ||
      (value.vendor_id != 0x1002 && value.vendor_id != 0x8086) ||
      !value.device_id || value.pci_domain > 0xffff || value.pci_device > 31 || value.pci_function > 7)
    return false;
  for (size_t i=sizeof(prefix)-1;i<value.render_node.size();++i)
    if (value.render_node[i] < '0' || value.render_node[i] > '9') return false;
  return true;
}
std::optional<LinuxVaapiDeviceSelection> LinuxVaapiSelection(
    const LinuxVaapiDevice& device) {
  unsigned domain=0,bus=0,slot=0,function=0;
  char extra=0;
  if (std::sscanf(device.pci.c_str(), "%x:%x:%x.%x%c",
                  &domain,&bus,&slot,&function,&extra) != 4 ||
      bus > 255 || slot > 31 || function > 7 || device.vendor > 0xffff ||
      device.device > 0xffff) return std::nullopt;
  LinuxVaapiDeviceSelection value;
  value.render_node=device.path;
  value.vendor_id=static_cast<uint16_t>(device.vendor);
  value.device_id=static_cast<uint16_t>(device.device);
  value.pci_domain=domain;value.pci_bus=static_cast<uint8_t>(bus);
  value.pci_device=static_cast<uint8_t>(slot);value.pci_function=static_cast<uint8_t>(function);
  return ValidLinuxVaapiSelection(value) ? std::optional(value) : std::nullopt;
}
bool SameLinuxVaapiSelection(const LinuxVaapiDeviceSelection& a,
                            const LinuxVaapiDeviceSelection& b) {
  return a.render_node==b.render_node && a.vendor_id==b.vendor_id &&
      a.device_id==b.device_id && a.pci_domain==b.pci_domain &&
      a.pci_bus==b.pci_bus && a.pci_device==b.pci_device &&
      a.pci_function==b.pci_function;
}
Status PinLinuxVaapiDevice(const LinuxVaapiDevice& device, int* owned_fd,
                          std::string* reason) {
  if (!owned_fd || *owned_fd != -1) return Status::kInvalidArgument;
  const auto expected=LinuxVaapiSelection(device);
  if (!expected) return Status::kInvalidArgument;
  const int fd=open(device.path.c_str(),O_RDWR|O_CLOEXEC);
  if(fd<0){
    if(reason)*reason=(errno==EACCES || errno==EPERM) ?
        "vaapi_render_node_permission_denied" : "vaapi_render_node_unavailable";
    return Status::kUnavailable;
  }
  struct stat attributes{};
  std::optional<LinuxVaapiDevice> actual;
  if(fstat(fd,&attributes)==0 && S_ISCHR(attributes.st_mode)) {
    const auto sysfs=std::string("/sys/dev/char/")+std::to_string(major(attributes.st_rdev))+":"+
        std::to_string(minor(attributes.st_rdev))+"/device";
    actual=ReadIdentity(sysfs,device.path);
  }
  const auto selection=actual ? LinuxVaapiSelection(*actual) : std::nullopt;
  if(!selection || !SameLinuxVaapiSelection(*expected,*selection)) {
    if(close(fd)!=0){
      RecordLinuxVaapiRetirement("vaapi_pin_retirement_failed");
      if(reason)*reason="vaapi_pin_retirement_failed";
      return Status::kRetirementFailed;
    }
    if(reason)*reason="vaapi_device_identity_changed";
    return Status::kUnavailable;
  }
  *owned_fd=fd;
  if(reason)reason->clear();
  return Status::kOk;
}
std::vector<LinuxVaapiDevice> EnumerateLinuxVaapiDevices() {
  std::vector<LinuxVaapiDevice> devices;
  DIR* directory=opendir("/dev/dri");
  if(!directory)return devices;
  while(auto* entry=readdir(directory)) {
    const char* name=entry->d_name;
    if(std::strncmp(name,"renderD",7)!=0 || !name[7])continue;
    bool numeric=true;
    for(const char* p=name+7;*p;++p)numeric &= *p>='0' && *p<='9';
    if(!numeric)continue;
    const auto sysfs=std::string("/sys/class/drm/")+name+"/device";
    auto device=ReadIdentity(sysfs,std::string("/dev/dri/")+name);
    if(device)devices.push_back(std::move(*device));
  }
  closedir(directory);
  std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){
    const int av=a.vendor==0x1002?0:1,bv=b.vendor==0x1002?0:1;
    if(av!=bv)return av<bv;
    return a.pci!=b.pci ? a.pci<b.pci : a.path<b.path;
  });
  return devices;
}
}  // namespace internal
}  // namespace webrtc::external_encoder
