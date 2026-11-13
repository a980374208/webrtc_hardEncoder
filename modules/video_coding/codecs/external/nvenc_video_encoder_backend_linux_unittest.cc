/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
// Linux-private dispatch-table contract tests. No GPU, encoder output, RTP,
// hardware retirement or Windows compatibility credit is granted by this fake.
#include "modules/video_coding/codecs/external/linux_nvenc_driver.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace webrtc::external_encoder {
namespace {
void Require(bool value) {
  if (!value) {
    std::cerr << "fake_driver_contract_failed\n";
    std::abort();
  }
}
struct Allocation { size_t pitch; std::vector<unsigned char> bytes; };
struct Output { uint64_t timestamp = 0; bool key = false; bool ready = false; };
struct State {
  std::vector<std::string> events;
  std::map<CUdeviceptr, Allocation> allocations;
  std::map<CUcontext, CUdevice> contexts;
  std::vector<CUcontext> stack{reinterpret_cast<CUcontext>(0x40)};
  std::map<void*, CUdeviceptr> registrations;
  std::map<void*, Output> outputs;
  int serial = 100;
  int device_count = 2;
  int register_calls = 0;
  int fail_register_call = 0;
  int fail_device = -1;
  bool fail_unregister = false;
  bool fail_destroy = false;
  bool fail_eos = false;
  bool fail_encode = false;
  bool encoder_busy = false;
  bool fail_reconfigure = false;
  bool mismatch_timestamp = false;
  bool malformed_capability = false;
  bool need_more_input = false;
  bool next_idr = false;
  bool swap_pci = false;
  bool session = false;
  int encode_calls = 0;
  int upload_calls = 0;
  int fail_upload_call = 0;
  int initialize_calls = 0;
  int reconfigure_calls = 0;
  uint32_t bitrate = 0;
  uint32_t fps = 0;
  std::vector<int> attempted_devices;
  uint32_t width = 0, height = 0;
  void* Handle() { return reinterpret_cast<void*>(static_cast<uintptr_t>(serial++)); }
  void Event(const char* value) { events.emplace_back(value); }
}* state;

CUresult CUDAAPI Init(unsigned) { state->Event("init"); return CUDA_SUCCESS; }
CUresult CUDAAPI Count(int* count) { *count=state->device_count; return CUDA_SUCCESS; }
CUresult CUDAAPI Device(CUdevice* device, int ordinal) { *device=ordinal; return CUDA_SUCCESS; }
CUresult CUDAAPI Pci(char* value,int size,CUdevice device) {
  // Device 1 sorts first. This catches hardcoded GPU 0 and ordinal-order probes.
  std::snprintf(value,static_cast<size_t>(size),"0000:%02d:00.0",state->swap_pci?device+1:2-device);
  return CUDA_SUCCESS;
}
CUresult CUDAAPI Current(CUcontext* context) { *context=state->stack.back(); return CUDA_SUCCESS; }
CUresult CUDAAPI ContextDevice(CUdevice* device) {
  Require(state->contexts.count(state->stack.back())==1);
  *device=state->contexts[state->stack.back()]; return CUDA_SUCCESS;
}
CUresult CUDAAPI Create(CUcontext* context,unsigned,CUdevice device) {
  *context=reinterpret_cast<CUcontext>(state->Handle());
  state->contexts[*context]=device;state->stack.push_back(*context);
  state->Event("context_create"); return CUDA_SUCCESS;
}
CUresult CUDAAPI Push(CUcontext context) {
  Require(state->contexts.count(context)==1);state->stack.push_back(context);return CUDA_SUCCESS;
}
CUresult CUDAAPI Pop(CUcontext* context) {
  Require(state->stack.size()>1);*context=state->stack.back();state->stack.pop_back();return CUDA_SUCCESS;
}
CUresult CUDAAPI DestroyContext(CUcontext context) {
  Require(state->stack.back()!=context && state->allocations.empty() && !state->session);
  state->contexts.erase(context);state->Event("context_destroy");return CUDA_SUCCESS;
}
CUresult CUDAAPI Allocate(CUdeviceptr* memory,size_t* pitch,size_t width,size_t height,unsigned) {
  *memory=reinterpret_cast<CUdeviceptr>(state->Handle());*pitch=(width+127)&~size_t{127};
  state->allocations.emplace(*memory,Allocation{*pitch,std::vector<unsigned char>(*pitch*height,0xcc)});
  state->Event("allocate");return CUDA_SUCCESS;
}
CUresult CUDAAPI Free(CUdeviceptr memory) {
  Require(!state->session && state->registrations.empty() && state->outputs.empty());
  Require(state->allocations.erase(memory)==1);state->Event("free");return CUDA_SUCCESS;
}
CUresult CUDAAPI Copy(const CUDA_MEMCPY2D* copy) {
  Require(copy->srcMemoryType==CU_MEMORYTYPE_HOST && copy->dstMemoryType==CU_MEMORYTYPE_DEVICE);
  Require(copy->WidthInBytes==state->width && copy->srcPitch>=copy->WidthInBytes);
  auto& memory=state->allocations.at(copy->dstDevice);
  Require(copy->dstPitch==memory.pitch && copy->dstXInBytes==0 && copy->srcY==0);
  const bool luma=(state->upload_calls%2)==0;
  if (state->fail_upload_call == state->upload_calls + 1) {
    ++state->upload_calls; return CUDA_ERROR_UNKNOWN;
  }
  Require(copy->dstY==(luma?0:state->height));
  Require(copy->Height==(luma?state->height:state->height/2));
  const auto* bytes=static_cast<const unsigned char*>(copy->srcHost);
  for(size_t row=0;row<copy->Height;++row) {
    Require(bytes[row*copy->srcPitch]==(luma?0x28:0x85));
    const size_t offset=(row+copy->dstY)*memory.pitch;
    Require(offset+copy->WidthInBytes<=memory.bytes.size());
    std::memcpy(memory.bytes.data()+offset,bytes+row*copy->srcPitch,copy->WidthInBytes);
    Require(memory.bytes[offset+copy->WidthInBytes]==0xcc); // Never overwrite pitch padding.
  }
  ++state->upload_calls;state->Event(luma?"upload_y":"upload_uv");return CUDA_SUCCESS;
}
NVENCSTATUS NVENCAPI Open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* params,void** session) {
  Require(params->deviceType==NV_ENC_DEVICE_TYPE_CUDA && params->device==state->stack.back());
  const int device=state->contexts.at(state->stack.back());state->attempted_devices.push_back(device);
  if(device==state->fail_device)return NV_ENC_ERR_UNSUPPORTED_DEVICE;
  state->session=true;*session=state;state->Event("open");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI GuidCount(void*,uint32_t* count) {*count=1;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI CodecGuids(void*,GUID* values,uint32_t capacity,uint32_t* count) {
  Require(capacity==1);values[0]=NV_ENC_CODEC_H264_GUID;
  *count=state->malformed_capability?2:1;return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI ProfileCount(void*,GUID,uint32_t* count) {*count=1;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI ProfileGuids(void*,GUID,GUID* values,uint32_t capacity,uint32_t* count) {
  Require(capacity==1);values[0]=NV_ENC_H264_PROFILE_BASELINE_GUID;*count=1;return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI PresetGuids(void*,GUID,GUID* values,uint32_t capacity,uint32_t* count) {
  Require(capacity==1);values[0]=NV_ENC_PRESET_P4_GUID;*count=1;return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI FormatCount(void*,GUID,uint32_t* count) {*count=1;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI Formats(void*,GUID,NV_ENC_BUFFER_FORMAT* values,uint32_t capacity,uint32_t* count) {
  Require(capacity==1);values[0]=NV_ENC_BUFFER_FORMAT_NV12;*count=1;return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Caps(void*,GUID,NV_ENC_CAPS_PARAM* params,int* value) {
  switch(params->capsToQuery) {
    case NV_ENC_CAPS_WIDTH_MAX:case NV_ENC_CAPS_HEIGHT_MAX:*value=4096;break;
    case NV_ENC_CAPS_LEVEL_MIN:*value=10;break;
    case NV_ENC_CAPS_LEVEL_MAX:*value=62;break;
    case NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES:*value=NV_ENC_PARAMS_RC_CBR;break;
    default:*value=1;break;
  }return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Preset(void*,GUID,GUID,NV_ENC_TUNING_INFO tuning,NV_ENC_PRESET_CONFIG* params) {
  Require(tuning==NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);
  params->presetCfg={};params->presetCfg.version=NV_ENC_CONFIG_VER;return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Initialize(void*,NV_ENC_INITIALIZE_PARAMS* params) {
  Require(!params->enableEncodeAsync && params->enablePTD && params->frameRateDen==1);
  const auto& config=*params->encodeConfig;
  Require(config.frameIntervalP==1 && config.rcParams.rateControlMode==NV_ENC_PARAMS_RC_CBR);
  Require(!config.rcParams.enableLookahead && !config.rcParams.enableExtLookahead &&
          !config.rcParams.lookaheadDepth && config.rcParams.zeroReorderDelay);
  Require(config.encodeCodecConfig.h264Config.entropyCodingMode==NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC);
  Require(config.encodeCodecConfig.h264Config.repeatSPSPPS && !config.encodeCodecConfig.h264Config.disableSPSPPS);
  state->width=params->encodeWidth;state->height=params->encodeHeight;
  state->bitrate=config.rcParams.averageBitRate;state->fps=params->frameRateNum;
  ++state->initialize_calls;state->Event("initialize");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Register(void*,NV_ENC_REGISTER_RESOURCE* params) {
  ++state->register_calls;
  if(state->register_calls==state->fail_register_call)return NV_ENC_ERR_RESOURCE_REGISTER_FAILED;
  Require(params->resourceType==NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR);
  const auto memory=reinterpret_cast<CUdeviceptr>(params->resourceToRegister);
  Require(state->allocations.at(memory).pitch==params->pitch && params->bufferFormat==NV_ENC_BUFFER_FORMAT_NV12);
  params->registeredResource=state->Handle();state->registrations[params->registeredResource]=memory;
  state->Event("register");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Unregister(void*,NV_ENC_REGISTERED_PTR resource) {
  state->Event("unregister");if(state->fail_unregister)return NV_ENC_ERR_GENERIC;
  Require(state->registrations.erase(resource)==1);return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI CreateBitstream(void*,NV_ENC_CREATE_BITSTREAM_BUFFER* params) {
  params->bitstreamBuffer=state->Handle();state->outputs[params->bitstreamBuffer]={};
  state->Event("bitstream_create");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI DestroyBitstream(void*,NV_ENC_OUTPUT_PTR output) {
  Require(state->outputs.erase(output)==1);state->Event("bitstream_destroy");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Map(void*,NV_ENC_MAP_INPUT_RESOURCE* params) {
  Require(state->registrations.count(params->registeredResource)==1);
  params->mappedResource=params->registeredResource;params->mappedBufferFmt=NV_ENC_BUFFER_FORMAT_NV12;
  state->Event("map");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Unmap(void*,NV_ENC_INPUT_PTR input) {
  Require(state->registrations.count(input)==1);state->Event("unmap");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Encode(void*,NV_ENC_PIC_PARAMS* params) {
  if(params->encodePicFlags&NV_ENC_PIC_FLAG_EOS) {
    state->Event("eos");return state->fail_eos?NV_ENC_ERR_GENERIC:NV_ENC_SUCCESS;
  }
  Require(params->inputPitch==state->allocations.at(state->registrations.at(params->inputBuffer)).pitch);
  if(state->encoder_busy) {
    ++state->encode_calls;state->Event("encode_busy");
    return NV_ENC_ERR_ENCODER_BUSY; // Rejected: do not generate any output.
  }
  auto& output=state->outputs.at(params->outputBitstream);
  output.ready=true;
  output.timestamp=params->inputTimeStamp;
  output.key=state->next_idr || (params->encodePicFlags&NV_ENC_PIC_FLAG_FORCEIDR);
  if(output.key) Require((params->encodePicFlags&NV_ENC_PIC_FLAG_OUTPUT_SPSPPS) || state->next_idr);
  state->next_idr=false;++state->encode_calls;state->Event("encode");
  if(state->need_more_input)return NV_ENC_ERR_NEED_MORE_INPUT;
  return state->fail_encode?NV_ENC_ERR_GENERIC:NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Lock(void*,NV_ENC_LOCK_BITSTREAM* params) {
  static unsigned char bytes[]={0,0,0,1,0x65,1,2,3};
  const auto& output=state->outputs.at(params->outputBitstream);
  Require(output.ready); // An output rejected with ENCODER_BUSY cannot be locked.
  params->bitstreamBufferPtr=bytes;params->bitstreamSizeInBytes=sizeof(bytes);
  params->outputTimeStamp=output.timestamp+(state->mismatch_timestamp?1:0);
  params->pictureType=output.key?NV_ENC_PIC_TYPE_IDR:NV_ENC_PIC_TYPE_P;
  params->frameAvgQP=24;state->Event("lock");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI Unlock(void*,NV_ENC_OUTPUT_PTR) {state->Event("unlock");return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI Reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS* params) {
  ++state->reconfigure_calls;
  if(state->fail_reconfigure)return NV_ENC_ERR_GENERIC;
  Require(params->forceIDR && !params->reInitEncodeParams.enableEncodeAsync);
  state->bitrate=params->reInitEncodeParams.encodeConfig->rcParams.averageBitRate;
  state->fps=params->reInitEncodeParams.frameRateNum;state->next_idr=true;
  state->Event("reconfigure");return NV_ENC_SUCCESS;
}
NVENCSTATUS NVENCAPI DestroyEncoder(void*) {
  Require(state->registrations.empty() && state->outputs.empty());state->Event("destroy");
  if(state->fail_destroy)return NV_ENC_ERR_GENERIC;
  state->session=false;return NV_ENC_SUCCESS;
}
std::shared_ptr<internal::LinuxNvencDriver> Driver() {
  auto driver=std::make_shared<internal::LinuxNvencDriver>();
  driver->init=Init;driver->device_get_count=Count;driver->device_get=Device;
  driver->device_get_pci_bus_id=Pci;driver->context_create=Create;
  driver->context_get_current=Current;driver->context_get_device=ContextDevice;
  driver->context_push=Push;driver->context_pop=Pop;driver->context_destroy=DestroyContext;
  driver->memory_alloc_pitch=Allocate;driver->memory_free=Free;driver->copy_2d=Copy;
  driver->driver_api_version=(NVENCAPI_MAJOR_VERSION<<4)|NVENCAPI_MINOR_VERSION;
  auto& api=driver->encode;
  api.nvEncOpenEncodeSessionEx=Open;api.nvEncGetEncodeGUIDCount=GuidCount;
  api.nvEncGetEncodeGUIDs=CodecGuids;api.nvEncGetEncodeProfileGUIDCount=ProfileCount;
  api.nvEncGetEncodeProfileGUIDs=ProfileGuids;api.nvEncGetEncodePresetCount=ProfileCount;
  api.nvEncGetEncodePresetGUIDs=PresetGuids;api.nvEncGetInputFormatCount=FormatCount;
  api.nvEncGetInputFormats=Formats;api.nvEncGetEncodeCaps=Caps;
  api.nvEncGetEncodePresetConfigEx=Preset;api.nvEncInitializeEncoder=Initialize;
  api.nvEncRegisterResource=Register;api.nvEncUnregisterResource=Unregister;
  api.nvEncCreateBitstreamBuffer=CreateBitstream;api.nvEncDestroyBitstreamBuffer=DestroyBitstream;
  api.nvEncMapInputResource=Map;api.nvEncUnmapInputResource=Unmap;
  api.nvEncEncodePicture=Encode;api.nvEncLockBitstream=Lock;api.nvEncUnlockBitstream=Unlock;
  api.nvEncReconfigureEncoder=Reconfigure;api.nvEncDestroyEncoder=DestroyEncoder;
  Require(driver->HasRequiredFunctions());return driver;
}
Config Configuration() {Config c;c.width=64;c.height=48;c.bitrate_bps=200000;c.framerate_fps=30;return c;}
struct Input {
  std::vector<unsigned char> y=std::vector<unsigned char>(96*48,0x28);
  std::vector<unsigned char> uv=std::vector<unsigned char>(80*24,0x85);
  Frame frame{y.data(),uv.data(),96,80,64,48,12345};
};
void Empty() {
  Require(!state->session && state->allocations.empty() && state->registrations.empty() && state->outputs.empty());
  Require(state->contexts.empty() && state->stack.size()==1);
}
size_t EventIndex(const char* value) {
  const auto found=std::find(state->events.begin(),state->events.end(),value);
  Require(found!=state->events.end());return static_cast<size_t>(found-state->events.begin());
}
void Happy() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kOk);
  Require(state->attempted_devices==std::vector<int>{1} && state->register_calls==4);
  const auto info=backend->GetInfo();Require(info.implementation_name=="NVENC-CUDA-Linux" && info.is_hardware_accelerated);
  Input input;Packet first,second;
  Require(backend->Encode(input.frame,true,&first)==Status::kOk && first.key_frame && first.frame_id==12345);
  input.frame.id=12346;Require(backend->Encode(input.frame,false,&second)==Status::kOk && !second.key_frame);
  Require(first.bytes==second.bytes && first.bytes.data()!=second.bytes.data());
  Require(state->upload_calls==4 && state->register_calls==4);
  Require(backend->SetRates({150000,15})==Status::kOk && state->bitrate==150000 && state->fps==15);
  input.frame.id=12347;Require(backend->Encode(input.frame,false,&second)==Status::kOk && second.key_frame);
  backend->Close();Require(backend->LastCloseStatus()==Status::kOk);Empty();
  Require(EventIndex("eos")<EventIndex("unregister") && EventIndex("destroy")<EventIndex("free") && EventIndex("free")<EventIndex("context_destroy"));
  const auto calls=state->events.size();backend->Close();Require(state->events.size()==calls);
  const auto metrics=SnapshotLinuxHardwareEncoderMetrics();
  const auto& upload=metrics.transfers[static_cast<size_t>(LinuxEncoderMetricTransfer::kCudaUpload)];
  Require(upload.measurement_available && upload.started_calls==6 && upload.successful_calls==6);
  Require(upload.requested_bytes==13824 && upload.successful_bytes==13824 && upload.completed.samples==6);
  uint64_t count=0;for(const auto bucket:upload.completed.buckets)count+=bucket;
  Require(count==6 && !metrics.gpu_readback_bytes.has_value());
}
void UploadFailureMetrics() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kOk);state->fail_upload_call=2;
  Input input;Packet packet;
  Require(backend->Encode(input.frame,true,&packet)==Status::kError && packet.bytes.empty());
  Require(state->encode_calls==0 && state->upload_calls==2);
  backend->Close();Require(backend->LastCloseStatus()==Status::kOk);Empty();
  const auto metrics=SnapshotLinuxHardwareEncoderMetrics();
  const auto& upload=metrics.transfers[static_cast<size_t>(LinuxEncoderMetricTransfer::kCudaUpload)];
  Require(upload.started_calls==2 && upload.requested_bytes==4608 && upload.completed.samples==2);
  Require(upload.successful_calls==1 && upload.successful_bytes==3072 && !metrics.gpu_readback_bytes.has_value());
}
void ExplicitDevice() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver(),0);
  Require(backend->Open(Configuration())==Status::kOk && state->attempted_devices==std::vector<int>{0});
  backend->Close();Empty();
}
void CandidateFallback() {
  state->fail_device=1;auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kOk && state->attempted_devices==std::vector<int>({1,0}));
  backend->Close();Empty();
}
void NoDevice() {
  const auto before=SnapshotLinuxHardwareEncoderMetrics();
  state->device_count=0;auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kUnavailable);Empty();
  const auto after=SnapshotLinuxHardwareEncoderMetrics();
  const auto index=static_cast<size_t>(LinuxEncoderMetricBackend::kNvenc);
  Require(after.backends[index].created_instances==before.backends[index].created_instances+1 &&
      after.backends[index].open_attempts==before.backends[index].open_attempts+1 &&
      after.backends[index].open_succeeded==before.backends[index].open_succeeded);
  for(const auto kind:{LinuxEncoderMetricBackend::kQsv,LinuxEncoderMetricBackend::kVaapi}) {
    const auto other=static_cast<size_t>(kind);
    Require(after.backends[other].created_instances==before.backends[other].created_instances &&
        after.backends[other].open_attempts==before.backends[other].open_attempts);
  }
}
void MissingOrdinal() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver(),5);
  Require(backend->Open(Configuration())==Status::kUnavailable && state->attempted_devices.empty());Empty();
}
void MissingFunction() {
  auto driver=Driver();driver->copy_2d=nullptr;
  auto backend=internal::CreateLinuxNvencBackendForTesting(driver);
  Require(backend->Open(Configuration())==Status::kUnavailable && state->events.empty());Empty();
}
void OldApi() {
  auto driver=Driver();--driver->driver_api_version;
  auto backend=internal::CreateLinuxNvencBackendForTesting(driver);
  Require(backend->Open(Configuration())==Status::kUnsupported && state->events.empty());Empty();
}
void InvalidConfiguration() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());auto c=Configuration();c.width=63;
  Require(backend->Open(c)==Status::kInvalidArgument);c=Configuration();c.codec=Codec::kH265;
  Require(backend->Open(c)==Status::kUnsupported && state->events.empty());Empty();
}
void MalformedCapability() {
  state->malformed_capability=true;auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kUnsupported);Empty();
}
void PartialOpen() {
  state->fail_register_call=2;auto backend=internal::CreateLinuxNvencBackendForTesting(Driver(),0);
  Require(backend->Open(Configuration())==Status::kError);Empty();
  Require(EventIndex("destroy")<EventIndex("free"));
}
void TimestampMismatch() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());Require(backend->Open(Configuration())==Status::kOk);
  state->mismatch_timestamp=true;Input input;Packet packet;
  Require(backend->Encode(input.frame,true,&packet)==Status::kError && packet.bytes.empty());
  Require(backend->Encode(input.frame,true,&packet)==Status::kError && state->encode_calls==1);
  backend->Close();Empty();Require(EventIndex("unlock")<EventIndex("unmap"));
}
void EncodeFailure() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());Require(backend->Open(Configuration())==Status::kOk);
  state->fail_encode=true;Input input;Packet packet;
  Require(backend->Encode(input.frame,true,&packet)==Status::kError && packet.bytes.empty());
  backend->Close();Empty();
  Require(EventIndex("encode")<EventIndex("eos") && EventIndex("eos")<EventIndex("lock") && EventIndex("lock")<EventIndex("unlock") && EventIndex("unlock")<EventIndex("unmap") && EventIndex("unmap")<EventIndex("unregister"));
}
void NeedMoreInput() {state->need_more_input=true;EncodeFailure();}
void EncoderBusy() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kOk);
  state->encoder_busy=true;Input input;Packet packet;
  Require(backend->Encode(input.frame,true,&packet)==Status::kError && packet.bytes.empty());
  backend->Close();Require(backend->LastCloseStatus()==Status::kOk);Empty();
  Require(std::find(state->events.begin(),state->events.end(),"lock")==state->events.end());
  Require(std::find(state->events.begin(),state->events.end(),"eos")==state->events.end());
  Require(EventIndex("unmap")<EventIndex("unregister"));
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc)==Status::kOk);
}
void EncoderBusyAfterOutput() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kOk);
  Input input;Packet packet;
  Require(backend->Encode(input.frame,true,&packet)==Status::kOk);
  state->encoder_busy=true;++input.frame.id;
  Require(backend->Encode(input.frame,false,&packet)==Status::kError && packet.bytes.empty());
  backend->Close();Require(backend->LastCloseStatus()==Status::kOk);Empty();
  Require(std::count(state->events.begin(),state->events.end(),"lock")==1);
  Require(EventIndex("encode_busy")<EventIndex("eos"));
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc)==Status::kOk);
}
void ReconfigureFailure() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());Require(backend->Open(Configuration())==Status::kOk);
  state->fail_reconfigure=true;Require(backend->SetRates({100000,15})==Status::kError);
  Input input;Packet packet;Require(backend->Encode(input.frame,true,&packet)==Status::kError && state->encode_calls==0);
  backend->Close();Empty();
}
void CheckRetirementBlock(std::unique_ptr<VideoEncoderBackend>& backend) {
  backend->Close();Require(backend->LastCloseStatus()==Status::kRetirementFailed);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kNvenc)==Status::kRetirementFailed);
  Require(!state->allocations.empty() && !state->contexts.empty());
  Require(std::find(state->events.begin(),state->events.end(),"free")==state->events.end());
  const auto count=state->events.size();backend->Close();Require(count==state->events.size());
  backend.reset();Require(count==state->events.size());
  auto other=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(other->Open(Configuration())==Status::kRetirementFailed && count==state->events.size());
}
void UnregisterFailure() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());Require(backend->Open(Configuration())==Status::kOk);
  state->fail_unregister=true;CheckRetirementBlock(backend);Require(state->session);
}
void DestroyFailure() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());Require(backend->Open(Configuration())==Status::kOk);
  state->fail_destroy=true;CheckRetirementBlock(backend);Require(state->session);
}
void PartialOpenRetirementFailure() {
  state->fail_register_call=2;state->fail_unregister=true;
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());
  Require(backend->Open(Configuration())==Status::kRetirementFailed);CheckRetirementBlock(backend);
}
void EosFailure() {
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver());Require(backend->Open(Configuration())==Status::kOk);
  Input input;Packet packet;Require(backend->Encode(input.frame,true,&packet)==Status::kOk);
  state->fail_eos=true;CheckRetirementBlock(backend);
  Require(std::find(state->events.begin(),state->events.end(),"unregister")==state->events.end());
}
void ResolvedSelection() {
  auto collector=std::make_shared<internal::LinuxNvencDeviceCollector>();
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver(),-1,collector);
  Require(backend->Open(Configuration())==Status::kOk && !collector->RetiredSelection());
  backend->Close();auto selected=collector->RetiredSelection();
  Require(selected && selected->cuda_ordinal==1 && selected->pci_bus_id=="0000:01:00.0" && selected->cuda_driver_version==0 && selected->nvenc_api_version==Driver()->driver_api_version);
  auto pinned=internal::CreateLinuxNvencBackendForTesting(Driver(),*selected);
  Require(pinned->Open(Configuration())==Status::kOk && state->attempted_devices==std::vector<int>({1,1}));
  pinned->Close();Empty();
}
void ResolvedPciDrift() {
  auto collector=std::make_shared<internal::LinuxNvencDeviceCollector>();
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver(),-1,collector);
  Require(backend->Open(Configuration())==Status::kOk);backend->Close();auto selected=collector->RetiredSelection();Require(selected.has_value());
  state->swap_pci=true;const auto contexts=std::count(state->events.begin(),state->events.end(),"context_create");
  auto pinned=internal::CreateLinuxNvencBackendForTesting(Driver(),*selected);
  Require(pinned->Open(Configuration())==Status::kUnavailable && state->attempted_devices==std::vector<int>({1}));
  Require(std::count(state->events.begin(),state->events.end(),"context_create")==contexts);Empty();
}
void ResolvedNoFallback() {
  state->fail_device=1;
  auto pinned=internal::CreateLinuxNvencBackendForTesting(Driver(),LinuxNvencDeviceSelection{1,"0000:01:00.0"});
  Require(pinned->Open(Configuration())==Status::kUnavailable && state->attempted_devices==std::vector<int>({1}));Empty();
}
void ResolvedCloseFailure() {
  auto collector=std::make_shared<internal::LinuxNvencDeviceCollector>();
  auto backend=internal::CreateLinuxNvencBackendForTesting(Driver(),-1,collector);
  Require(backend->Open(Configuration())==Status::kOk);state->fail_destroy=true;backend->Close();
  Require(backend->LastCloseStatus()==Status::kRetirementFailed && !collector->RetiredSelection());
}
void ResolvedInvalid() {
  auto pinned=internal::CreateLinuxNvencBackendForTesting(Driver(),LinuxNvencDeviceSelection{-1,"0000:01:00.0"});
  Require(pinned->Open(Configuration())==Status::kInvalidArgument && state->events.empty());Empty();
}
void LoaderAbsent() {
  Status status;std::string reason;
  auto driver=internal::LoadLinuxNvencDriverForTesting("/cohavora-absent-cuda.so","/cohavora-absent-nvenc.so",&status,&reason);
  Require(!driver && status==Status::kUnavailable && reason=="cuda_library_unavailable");
}
void LoaderMissingEntry() {
  Status status;std::string reason;
  auto driver=internal::LoadLinuxNvencDriverForTesting("libc.so.6","libc.so.6",&status,&reason);
  Require(!driver && status==Status::kUnavailable && reason=="cuda_entry_point_missing");
}
}  // namespace
}  // namespace webrtc::external_encoder

int main() {
  using namespace webrtc::external_encoder;
  struct Test {const char* name;void (*run)();};
  const Test tests[]={{"happy_pitch_rates_owned_output_release",Happy},
    {"failed_chroma_upload_metrics_only_successful_luma_bytes",UploadFailureMetrics},{"explicit_device",ExplicitDevice},
    {"stable_candidate_fallback",CandidateFallback},{"no_device",NoDevice},{"missing_ordinal",MissingOrdinal},
    {"missing_function",MissingFunction},{"old_api",OldApi},{"invalid_configuration",InvalidConfiguration},
    {"malformed_capability_count",MalformedCapability},{"partial_open_cleanup",PartialOpen},
    {"timestamp_mismatch",TimestampMismatch},{"encode_error_retirement_order",EncodeFailure},
    {"delayed_output_rejected",NeedMoreInput},{"encoder_busy_no_output",EncoderBusy},
    {"encoder_busy_after_output",EncoderBusyAfterOutput},{"rate_error_poison",ReconfigureFailure},
    {"unregister_failure_retained",UnregisterFailure},{"destroy_failure_retained",DestroyFailure},
    {"partial_open_retirement_poison",PartialOpenRetirementFailure},{"eos_failure_retained",EosFailure},
    {"resolved_selection_confirmed_close",ResolvedSelection},{"resolved_ordinal_pci_drift_rejected",ResolvedPciDrift},
    {"resolved_no_device_fallback",ResolvedNoFallback},{"resolved_close_failure_no_output",ResolvedCloseFailure},
    {"resolved_invalid_no_driver",ResolvedInvalid},{"loader_absent_library",LoaderAbsent},{"loader_missing_entry",LoaderMissingEntry}};
  int passed=0;
  for(const auto& test:tests) {
    const pid_t child=fork();
    if(child==0) {State local;state=&local;test.run();std::_Exit(0);}
    Require(child>0);int result=0;Require(waitpid(child,&result,0)==child);
    if(!WIFEXITED(result)||WEXITSTATUS(result)!=0) {std::cerr<<test.name<<" FAIL\n";return 1;}
    std::cout<<test.name<<" PASS\n";++passed;
  }
  std::cout<<"linux_nvenc_backend_fake_tests PASS checks="<<passed<<" hardware=NOT_RUN\n";
  return 0;
}
