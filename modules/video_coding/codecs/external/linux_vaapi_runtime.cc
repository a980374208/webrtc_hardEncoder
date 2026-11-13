// Built as a separate Linux DSO. Its FFmpeg implementation is private/hidden.
#define COHAVORA_VAAPI_RUNTIME_IMPLEMENTATION
#include "modules/video_coding/codecs/external/linux_vaapi_runtime.h"
#include "modules/video_coding/codecs/external/linux_vaapi_runtime_guard.h"
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <new>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}
using cohavora_vaapi::GuardedNative;
using cohavora_vaapi::RetirementScope;
using cohavora_vaapi::RetirementState;

// FFmpeg's pinned mem.c invokes this before every av_free. It is deliberately
// private to this DSO. After a native cleanup failure, subsequent metadata and
// CPU storage stay allocated alongside the retained session/display/FD/DSO.
extern "C" int cohavora_vaapi_retain_host_storage() {
  return cohavora_vaapi::RetainHostStorage();
}

// FFmpeg ignores several native destroy statuses. Guard all of those calls,
// and stop dependent native destruction after the first failure. A thread/TLS
// scope owns every public operation; FFmpeg frame/codec threads are disabled.
#define WRAP_NATIVE(name, args, callargs) \
extern "C" VAStatus __real_##name args; \
extern "C" VAStatus __wrap_##name args { \
  const int status = GuardedNative([&] { return int(__real_##name callargs); }); \
  return status < 0 ? VA_STATUS_ERROR_OPERATION_FAILED : VAStatus(status); \
}
WRAP_NATIVE(vaDestroyContext, (VADisplay d, VAContextID id), (d,id))
WRAP_NATIVE(vaDestroyConfig, (VADisplay d, VAConfigID id), (d,id))
WRAP_NATIVE(vaDestroyBuffer, (VADisplay d, VABufferID id), (d,id))
WRAP_NATIVE(vaDestroySurfaces, (VADisplay d, VASurfaceID* ids, int n), (d,ids,n))
WRAP_NATIVE(vaDestroyImage, (VADisplay d, VAImageID id), (d,id))
WRAP_NATIVE(vaUnmapBuffer, (VADisplay d, VABufferID id), (d,id))
WRAP_NATIVE(vaPutImage,
    (VADisplay d, VASurfaceID surface, VAImageID image,
     int sx, int sy, unsigned sw, unsigned sh,
     int dx, int dy, unsigned dw, unsigned dh),
    (d,surface,image,sx,sy,sw,sh,dx,dy,dw,dh))
WRAP_NATIVE(vaTerminate, (VADisplay d), (d))
WRAP_NATIVE(vaSyncSurface, (VADisplay d, VASurfaceID id), (d,id))
#undef WRAP_NATIVE
extern "C" VAStatus __real_vaSyncBuffer(VADisplay, VABufferID, uint64_t);
extern "C" VAStatus __wrap_vaSyncBuffer(VADisplay d, VABufferID id, uint64_t timeout) {
  // FFmpeg uses this invalid-ID call only to discover support. Its expected
  // non-success result is not an outstanding encoder resource or cleanup error.
  if (id == VA_INVALID_ID && timeout == 0)
    return __real_vaSyncBuffer(d,id,timeout);
  const int status = GuardedNative([&] {
    return int(__real_vaSyncBuffer(d,id,timeout));
  });
  return status < 0 ? VA_STATUS_ERROR_OPERATION_FAILED : VAStatus(status);
}
namespace {
using Status = CohavoraVaapiStatus;
// Independent fixed storage inside this hidden DSO. No SDK sink, callback,
// mutex, heap allocation or vendor call is needed for process snapshots.
using MetricClock = std::chrono::steady_clock;
constexpr std::array<uint64_t, COHAVORA_VAAPI_DURATION_BUCKETS> kDurationBounds = {
    1000,10000,100000,1000000,10000000,100000000,1000000000,UINT64_MAX};
struct AtomicDuration {
  std::atomic<uint64_t> samples{0}, total_ns{0}, max_ns{0};
  std::array<std::atomic<uint64_t>, COHAVORA_VAAPI_DURATION_BUCKETS> buckets{};
  void Add(uint64_t ns) {
    samples.fetch_add(1,std::memory_order_relaxed);total_ns.fetch_add(ns,std::memory_order_relaxed);
    auto prior=max_ns.load(std::memory_order_relaxed);
    while(prior<ns && !max_ns.compare_exchange_weak(prior,ns,std::memory_order_relaxed)) {}
    for(size_t i=0;i<kDurationBounds.size();++i) if(ns<=kDurationBounds[i]) {
      buckets[i].fetch_add(1,std::memory_order_relaxed);break;
    }
  }
  CohavoraVaapiDuration Snapshot() const {
    CohavoraVaapiDuration value{};
    value.samples=samples.load(std::memory_order_relaxed);
    value.total_ns=total_ns.load(std::memory_order_relaxed);value.max_ns=max_ns.load(std::memory_order_relaxed);
    for(size_t i=0;i<kDurationBounds.size();++i)value.buckets[i]=buckets[i].load(std::memory_order_relaxed);
    return value;
  }
};
struct AtomicTransfer {
  std::atomic<uint64_t> started{0}, requested{0}, succeeded{0}, successful_bytes{0};
  AtomicDuration completed;
  CohavoraVaapiTransfer Snapshot() const {
    return {started.load(std::memory_order_relaxed),requested.load(std::memory_order_relaxed),
        succeeded.load(std::memory_order_relaxed),successful_bytes.load(std::memory_order_relaxed),completed.Snapshot()};
  }
};
struct ProcessMetrics {
  AtomicTransfer cpu_copy, upload;
  std::atomic<uint64_t> rebuild_started{0},rebuild_succeeded{0},rebuild_failed{0};
} process_metrics;
class TransferTimer {
 public:
  TransferTimer(AtomicTransfer& metric,uint64_t bytes):metric_(metric),bytes_(bytes),start_(MetricClock::now()) {
    metric_.started.fetch_add(1,std::memory_order_relaxed);metric_.requested.fetch_add(bytes,std::memory_order_relaxed);
  }
  ~TransferTimer(){if(!finished_)Finish(false);}
  void Finish(bool successful) {
    if(finished_)return;finished_=true;
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(MetricClock::now()-start_).count();
    metric_.completed.Add(uint64_t(std::max<int64_t>(0,ns)));
    if(successful){metric_.succeeded.fetch_add(1,std::memory_order_relaxed);metric_.successful_bytes.fetch_add(bytes_,std::memory_order_relaxed);}
  }
 private:
  AtomicTransfer& metric_;uint64_t bytes_;MetricClock::time_point start_;bool finished_=false;
};
Status MetricsImpl(CohavoraVaapiMetrics* output) {
  if(!output || output->struct_size<sizeof(*output))return COHAVORA_VAAPI_INVALID_ARGUMENT;
  *output={sizeof(*output),COHAVORA_VAAPI_METRICS_SCHEMA_VERSION,COHAVORA_VAAPI_METRICS_PROCESS_SCOPE,0,
      process_metrics.cpu_copy.Snapshot(),process_metrics.upload.Snapshot(),
      process_metrics.rebuild_started.load(std::memory_order_relaxed),
      process_metrics.rebuild_succeeded.load(std::memory_order_relaxed),
      process_metrics.rebuild_failed.load(std::memory_order_relaxed),0};
  return COHAVORA_VAAPI_OK;
}
struct Session {
  RetirementState retirement;
  CohavoraVaapiConfig config{};
  char device_path[256]{};
  int fd = -1;
  VADisplay display = nullptr;
  AVBufferRef* device = nullptr;
  AVBufferRef* frames = nullptr;
  AVCodecContext* codec = nullptr;
  AVFrame* cpu = nullptr;
  AVFrame* upload = nullptr;
  AVPacket* packet = nullptr;
  bool next_idr = true;
  bool unhealthy = false;
};
pthread_once_t quiet_once = PTHREAD_ONCE_INIT;
void QuietLog(void*, int, const char*, va_list) {}
void InstallQuietLog() { av_log_set_callback(QuietLog); }
bool ValidRates(uint32_t bitrate, uint32_t fps) {
  return bitrate > 0 && bitrate <= uint32_t(std::numeric_limits<int>::max()) &&
      fps > 0 && fps <= 240;
}
bool ValidConfig(const CohavoraVaapiConfig* config) {
  return config && config->struct_size == sizeof(*config) &&
      config->width && config->height && config->width <= 8192 &&
      config->height <= 8192 && !(config->width & 1) && !(config->height & 1) &&
      ValidRates(config->bitrate_bps, config->framerate_fps) &&
      config->device_path && config->device_path[0] == '/' &&
      std::strlen(config->device_path) < sizeof(Session::device_path);
}
Status OpenCodec(Session& s) {
  const AVCodec* encoder = avcodec_find_encoder_by_name("h264_vaapi");
  if (!encoder) return COHAVORA_VAAPI_UNAVAILABLE;
  s.codec = avcodec_alloc_context3(encoder);
  if (!s.codec) return COHAVORA_VAAPI_ERROR;
  auto* c = s.codec;
  c->width = int(s.config.width); c->height = int(s.config.height);
  c->pix_fmt = AV_PIX_FMT_VAAPI;
  c->sw_pix_fmt = AV_PIX_FMT_NV12;
  c->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
  c->level = int(s.config.h264_level);
  c->time_base = {1, int(s.config.framerate_fps)};
  c->framerate = {int(s.config.framerate_fps), 1};
  c->bit_rate = s.config.bitrate_bps;
  c->rc_max_rate = s.config.bitrate_bps;
  c->rc_buffer_size = std::max(1, int(s.config.bitrate_bps / s.config.framerate_fps));
  c->gop_size = std::min(int(s.config.framerate_fps) * 2, 480);
  c->max_b_frames = 0; c->refs = 1;
  c->thread_count = 1; c->thread_type = 0;
  c->flags |= AV_CODEC_FLAG_LOW_DELAY;
  c->hw_frames_ctx = av_buffer_ref(s.frames);
  if (!c->hw_frames_ctx) return COHAVORA_VAAPI_ERROR;
  if (av_opt_set_int(c->priv_data, "async_depth", 1, 0) < 0 ||
      av_opt_set_int(c->priv_data, "coder", 0, 0) < 0 ||
      av_opt_set(c->priv_data, "rc_mode", "CBR", 0) < 0)
    return COHAVORA_VAAPI_UNSUPPORTED;
  const int status = avcodec_open2(c, encoder, nullptr);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  if (status < 0) return COHAVORA_VAAPI_UNSUPPORTED;
  // The fixed no-B, async-depth-one configuration permits same-frame output.
  // Actual receive EAGAIN/identity mismatch remains a runtime rejection.
  s.next_idr = true;
  return COHAVORA_VAAPI_OK;
}
Status CloseImpl(void** handle) {
  if (!handle) return COHAVORA_VAAPI_INVALID_ARGUMENT;
  if (!*handle) return COHAVORA_VAAPI_OK;
  auto& s = *static_cast<Session*>(*handle);
  if (!s.retirement.OnOwner()) return COHAVORA_VAAPI_INVALID_ARGUMENT;
  RetirementScope scope(s.retirement);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  avcodec_free_context(&s.codec);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  av_frame_free(&s.upload);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  av_packet_free(&s.packet);
  av_frame_free(&s.cpu);
  av_buffer_unref(&s.frames);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  // FFmpeg only borrowed this display; its AVHWDeviceContext has no external
  // free callback. We retain display and fd if any earlier native call failed.
  av_buffer_unref(&s.device);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  if (s.display) {
    if (__wrap_vaTerminate(s.display) != VA_STATUS_SUCCESS)
      return COHAVORA_VAAPI_RETIREMENT_FAILED;
    s.display = nullptr;
  }
  if (s.fd >= 0) {
    if (GuardedNative([&] { return ::close(s.fd); }) != 0)
      return COHAVORA_VAAPI_RETIREMENT_FAILED;
    s.fd = -1;
  }
  delete &s;
  *handle = nullptr;
  return COHAVORA_VAAPI_OK;
}
Status OpenImpl(const CohavoraVaapiConfig* config, void** handle) {
  if (!handle || *handle || !ValidConfig(config)) return COHAVORA_VAAPI_INVALID_ARGUMENT;
  pthread_once(&quiet_once, InstallQuietLog);
  auto* s = new (std::nothrow) Session;
  if (!s) return COHAVORA_VAAPI_ERROR;
  *handle = s;
  s->config = *config;
  std::strcpy(s->device_path, config->device_path);
  s->config.device_path = s->device_path;
  RetirementScope scope(s->retirement);
  auto fail = [&](Status status) {
    if (s->retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
    const auto close = CloseImpl(handle);
    return close == COHAVORA_VAAPI_OK ? status : COHAVORA_VAAPI_RETIREMENT_FAILED;
  };
  s->fd = ::open(s->device_path, O_RDWR | O_CLOEXEC);
  if (s->fd < 0) return fail(COHAVORA_VAAPI_UNAVAILABLE);
  s->display = vaGetDisplayDRM(s->fd);
  if (!s->display) return fail(COHAVORA_VAAPI_UNAVAILABLE);
  int major = 0, minor = 0;
  if (vaInitialize(s->display, &major, &minor) != VA_STATUS_SUCCESS)
    return fail(COHAVORA_VAAPI_UNAVAILABLE);
  s->device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
  if (!s->device) return fail(COHAVORA_VAAPI_ERROR);
  auto* hwdevice = reinterpret_cast<AVHWDeviceContext*>(s->device->data);
  reinterpret_cast<AVVAAPIDeviceContext*>(hwdevice->hwctx)->display = s->display;
  if (av_hwdevice_ctx_init(s->device) < 0) return fail(COHAVORA_VAAPI_UNSUPPORTED);
  s->frames = av_hwframe_ctx_alloc(s->device);
  if (!s->frames) return fail(COHAVORA_VAAPI_ERROR);
  auto* frames = reinterpret_cast<AVHWFramesContext*>(s->frames->data);
  frames->format = AV_PIX_FMT_VAAPI; frames->sw_format = AV_PIX_FMT_NV12;
  frames->width = int(config->width); frames->height = int(config->height);
  frames->initial_pool_size = 4;
  if (av_hwframe_ctx_init(s->frames) < 0) return fail(COHAVORA_VAAPI_UNSUPPORTED);
  s->cpu = av_frame_alloc(); s->upload = av_frame_alloc(); s->packet = av_packet_alloc();
  if (!s->cpu || !s->upload || !s->packet) return fail(COHAVORA_VAAPI_ERROR);
  s->cpu->format = AV_PIX_FMT_NV12;
  s->cpu->width = int(config->width); s->cpu->height = int(config->height);
  if (av_frame_get_buffer(s->cpu, 32) < 0) return fail(COHAVORA_VAAPI_ERROR);
  const auto status = OpenCodec(*s);
  return status == COHAVORA_VAAPI_OK ? status : fail(status);
}
Status RatesImpl(void* handle, uint32_t bitrate, uint32_t fps) {
  if (!handle || !ValidRates(bitrate, fps)) return COHAVORA_VAAPI_INVALID_ARGUMENT;
  auto& s = *static_cast<Session*>(handle);
  if (!s.retirement.OnOwner()) return COHAVORA_VAAPI_INVALID_ARGUMENT;
  RetirementScope scope(s.retirement);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  if (s.unhealthy || !s.codec) return COHAVORA_VAAPI_ERROR;
  if (bitrate == s.config.bitrate_bps && fps == s.config.framerate_fps)
    return COHAVORA_VAAPI_OK;
  // The pinned encoder does not promise live bitrate updates. Rebuild only
  // after the preceding synchronous output and confirm old resource retirement.
  process_metrics.rebuild_started.fetch_add(1,std::memory_order_relaxed);
  avcodec_free_context(&s.codec);
  if (s.retirement.failed) {
    process_metrics.rebuild_failed.fetch_add(1,std::memory_order_relaxed);
    return COHAVORA_VAAPI_RETIREMENT_FAILED;
  }
  s.config.bitrate_bps = bitrate; s.config.framerate_fps = fps;
  const auto status = OpenCodec(s);
  if (status != COHAVORA_VAAPI_OK) s.unhealthy = true;
  (status==COHAVORA_VAAPI_OK ? process_metrics.rebuild_succeeded : process_metrics.rebuild_failed).fetch_add(1,std::memory_order_relaxed);
  return status;
}
Status EncodeImpl(void* handle, const CohavoraVaapiFrame* frame, uint32_t force_key,
                  CohavoraVaapiPacket* output) {
  if (!handle || !frame || frame->struct_size != sizeof(*frame) || !output ||
      output->struct_size != sizeof(*output)) return COHAVORA_VAAPI_INVALID_ARGUMENT;
  output->size = 0; output->frame_id = 0; output->key_frame = 0; output->qp = -1;
  auto& s = *static_cast<Session*>(handle);
  if (!s.retirement.OnOwner()) return COHAVORA_VAAPI_INVALID_ARGUMENT;
  RetirementScope scope(s.retirement);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  if (!s.codec || s.unhealthy || !frame->y || !frame->uv || !output->data ||
      frame->width != s.config.width || frame->height != s.config.height ||
      frame->stride_y < int(frame->width) || frame->stride_uv < int(frame->width) ||
      frame->frame_id > uint64_t(std::numeric_limits<int64_t>::max()))
    return COHAVORA_VAAPI_INVALID_ARGUMENT;
  auto fail = [&](Status status) {
    s.unhealthy = true;
    return s.retirement.failed ? COHAVORA_VAAPI_RETIREMENT_FAILED : status;
  };
  if (av_frame_make_writable(s.cpu) < 0) return fail(COHAVORA_VAAPI_ERROR);
  const uint64_t nv12_bytes=uint64_t(frame->width)*frame->height*3/2;
  TransferTimer cpu_copy(process_metrics.cpu_copy,nv12_bytes);
  for (uint32_t row = 0; row < frame->height; ++row)
    std::memcpy(s.cpu->data[0] + size_t(row) * s.cpu->linesize[0],
        frame->y + size_t(row) * frame->stride_y, frame->width);
  for (uint32_t row = 0; row < frame->height / 2; ++row)
    std::memcpy(s.cpu->data[1] + size_t(row) * s.cpu->linesize[1],
        frame->uv + size_t(row) * frame->stride_uv, frame->width);
  cpu_copy.Finish(true);
  av_frame_unref(s.upload);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  if (av_hwframe_get_buffer(s.frames, s.upload, 0) < 0)
    return fail(COHAVORA_VAAPI_ERROR);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  TransferTimer upload(process_metrics.upload,nv12_bytes);
  const int upload_result=av_hwframe_transfer_data(s.upload,s.cpu,0);
  upload.Finish(upload_result>=0 && !s.retirement.failed);
  if(upload_result<0)return fail(COHAVORA_VAAPI_ERROR);
  // FFmpeg's void unmap callback can swallow Unmap/PutImage/DestroyImage
  // failures. A successful transfer return is insufficient evidence of upload.
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  const bool key = force_key || s.next_idr;
  s.upload->pts = int64_t(frame->frame_id);
  s.upload->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
  s.upload->duration = 1;
  av_packet_unref(s.packet);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  int status = avcodec_send_frame(s.codec, s.upload);
  // The fixed encoder may likewise ignore parameter-buffer destruction errors.
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  if (status < 0) return fail(status == AVERROR(EAGAIN) ?
      COHAVORA_VAAPI_NO_OUTPUT : COHAVORA_VAAPI_ERROR);
  status = avcodec_receive_packet(s.codec, s.packet);
  if (status < 0) return fail(status == AVERROR(EAGAIN) ?
      COHAVORA_VAAPI_NO_OUTPUT : COHAVORA_VAAPI_ERROR);
  if (s.retirement.failed) return COHAVORA_VAAPI_RETIREMENT_FAILED;
  const auto* p = s.packet;
  const bool annexb = p->size >= 4 && p->data[0] == 0 && p->data[1] == 0 &&
      (p->data[2] == 1 || (p->data[2] == 0 && p->data[3] == 1));
  if (p->size <= 0 || size_t(p->size) > output->capacity ||
      p->pts != int64_t(frame->frame_id) || !annexb ||
      (key && !(p->flags & AV_PKT_FLAG_KEY)))
    return fail(COHAVORA_VAAPI_UNSUPPORTED);
  std::memcpy(output->data, p->data, size_t(p->size));
  output->size = size_t(p->size); output->frame_id = frame->frame_id;
  output->key_frame = (p->flags & AV_PKT_FLAG_KEY) != 0;
  av_packet_unref(s.packet);
  s.next_idr = false;
  return COHAVORA_VAAPI_OK;
}
}  // namespace
static_assert(offsetof(CohavoraVaapiApi,get_metrics)==sizeof(CohavoraVaapiApiV1));
extern "C" CohavoraVaapiStatus cohavora_vaapi_get_api(uint32_t abi, void* output) {
  if(!output)return COHAVORA_VAAPI_UNSUPPORTED;
  uint32_t size=0;std::memcpy(&size,output,sizeof(size));
  if(abi==COHAVORA_VAAPI_ABI_VERSION_1 && size>=sizeof(CohavoraVaapiApiV1)) {
    const CohavoraVaapiApiV1 prefix{sizeof(prefix),COHAVORA_VAAPI_ABI_VERSION_1,
        OpenImpl,RatesImpl,EncodeImpl,CloseImpl};
    std::memcpy(output,&prefix,sizeof(prefix));
    return COHAVORA_VAAPI_OK;
  }
  if(abi!=COHAVORA_VAAPI_ABI_VERSION || size<sizeof(CohavoraVaapiApi))
    return COHAVORA_VAAPI_UNSUPPORTED;
  const CohavoraVaapiApi api{sizeof(api),COHAVORA_VAAPI_ABI_VERSION,
      OpenImpl,RatesImpl,EncodeImpl,CloseImpl,MetricsImpl};
  std::memcpy(output,&api,sizeof(api));
  return COHAVORA_VAAPI_OK;
}
