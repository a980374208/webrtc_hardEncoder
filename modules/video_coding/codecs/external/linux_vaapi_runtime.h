/* Linux-only C ABI. No FFmpeg or libva types cross this boundary. */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_VAAPI_RUNTIME_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_VAAPI_RUNTIME_H_
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define COHAVORA_VAAPI_ABI_VERSION 2u
#define COHAVORA_VAAPI_ABI_VERSION_1 1u
#define COHAVORA_VAAPI_RUNTIME_SONAME "libcohavora_vaapi_runtime.so.1"
typedef enum CohavoraVaapiStatus {
  COHAVORA_VAAPI_OK = 0,
  COHAVORA_VAAPI_UNAVAILABLE = 1,
  COHAVORA_VAAPI_UNSUPPORTED = 2,
  COHAVORA_VAAPI_INVALID_ARGUMENT = 3,
  COHAVORA_VAAPI_NO_OUTPUT = 4,
  COHAVORA_VAAPI_ERROR = 5,
  COHAVORA_VAAPI_RETIREMENT_FAILED = 6
} CohavoraVaapiStatus;
typedef struct CohavoraVaapiConfig {
  uint32_t struct_size;
  uint32_t width, height, bitrate_bps, framerate_fps, h264_level;
  const char* device_path; /* Borrowed only by open; selected render node. */
} CohavoraVaapiConfig;
typedef struct CohavoraVaapiFrame {
  uint32_t struct_size;
  const uint8_t* y;
  const uint8_t* uv;
  int32_t stride_y, stride_uv;
  uint32_t width, height;
  uint64_t frame_id;
} CohavoraVaapiFrame;
typedef struct CohavoraVaapiPacket {
  uint32_t struct_size;
  uint8_t* data;
  size_t capacity, size;
  uint64_t frame_id;
  uint32_t key_frame;
  int32_t qp;
} CohavoraVaapiPacket;
/* Fixed-capacity, observational process counters. Snapshot fields are not a
 * transaction and include failed attempts; no driver/session strings cross. */
#define COHAVORA_VAAPI_METRICS_SCHEMA_VERSION 1u
#define COHAVORA_VAAPI_METRICS_PROCESS_SCOPE 1u
#define COHAVORA_VAAPI_DURATION_BUCKETS 8u
typedef struct CohavoraVaapiDuration {
  uint64_t samples, total_ns, max_ns;
  uint64_t buckets[COHAVORA_VAAPI_DURATION_BUCKETS];
} CohavoraVaapiDuration;
typedef struct CohavoraVaapiTransfer {
  uint64_t started_calls, requested_bytes, successful_calls, successful_bytes;
  CohavoraVaapiDuration completed;
} CohavoraVaapiTransfer;
typedef struct CohavoraVaapiMetrics {
  uint32_t struct_size, schema_version, scope, gpu_readback_known;
  CohavoraVaapiTransfer cpu_nv12_copy, upload;
  uint64_t rebuild_started, rebuild_succeeded, rebuild_failed;
  /* Unknown unless gpu_readback_known == 1; never infer measured zero. */
  uint64_t gpu_readback_bytes;
} CohavoraVaapiMetrics;
/* Preserve ABI1 as an explicit table. get_api(1, old-prefix-buffer) writes only
 * sizeof(CohavoraVaapiApiV1), even when the caller supplies a larger buffer. */
typedef struct CohavoraVaapiApiV1 {
  uint32_t struct_size;
  uint32_t abi_version;
  CohavoraVaapiStatus (*open)(const CohavoraVaapiConfig*, void**);
  CohavoraVaapiStatus (*set_rates)(void*, uint32_t, uint32_t);
  CohavoraVaapiStatus (*encode)(void*, const CohavoraVaapiFrame*, uint32_t,
                               CohavoraVaapiPacket*);
  CohavoraVaapiStatus (*close)(void**);
} CohavoraVaapiApiV1;
typedef struct CohavoraVaapiApi {
  uint32_t struct_size;
  uint32_t abi_version;
  CohavoraVaapiStatus (*open)(const CohavoraVaapiConfig*, void**);
  CohavoraVaapiStatus (*set_rates)(void*, uint32_t, uint32_t);
  CohavoraVaapiStatus (*encode)(void*, const CohavoraVaapiFrame*, uint32_t,
                               CohavoraVaapiPacket*);
  /* Success destroys/clears handle. Failure retains handle and native storage. */
  CohavoraVaapiStatus (*close)(void**);
  /* ABI2: independent of sessions; no GPU access, allocation or blocking. */
  CohavoraVaapiStatus (*get_metrics)(CohavoraVaapiMetrics*);
} CohavoraVaapiApi;
typedef CohavoraVaapiStatus (*CohavoraVaapiGetApi)(uint32_t, void*);
#if defined(COHAVORA_VAAPI_RUNTIME_IMPLEMENTATION)
__attribute__((visibility("default")))
#endif
CohavoraVaapiStatus cohavora_vaapi_get_api(uint32_t, void*);
#ifdef __cplusplus
}
#endif
#endif
