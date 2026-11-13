/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/linux_qsv_driver.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_metrics.h"
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace webrtc::external_encoder {
namespace {
void Require(bool value) { if (!value) std::abort(); }
struct OpenGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered=false, released=false;
};
struct State {
  std::shared_ptr<OpenGate> open_gate;
  std::array<drmDevice,3> devices{};
  std::array<drmPciBusInfo,3> buses{};
  std::array<drmPciDeviceInfo,3> ids{};
  std::array<std::array<char*,DRM_NODE_MAX>,3> nodes{};
  char node0[32]="/dev/dri/renderD129", node1[32]="/dev/dri/renderD128", node2[32]="/dev/dri/renderD130";
  std::array<mfxImplDescription,3> descriptions{};
  std::array<mfxExtendedDeviceId,3> extended{};
  std::map<std::string,mfxU32> filters;
  std::vector<std::string> events, attempted;
  bool owner_set=false; pthread_t owner{};
  int device_count=3, fail_open_index=-1, active_device=-1, selected_impl=-1;
  int fail_open_errno=ENOENT;
  int open_count=0, close_count=0, encode_count=0, sync_count=0, reset_count=0, header_count=0;
  bool fd_open=false, va_active=false, loader=false, session=false, initialized=false;
  bool bad_identity=false, no_extended=false, software_only=false, wrong_runtime=false, wrong_display=false;
  bool fail_initialize=false, wrong_profile=false, fail_reset=false, malformed_headers=false;
  int pool_min=1, pool_suggested=1, pool_queries=0;
  uint8_t expected_y=0x28;
  bool reset_locks_surface=false;
  std::vector<mfxFrameSurface1*> submitted_surfaces, locked_surfaces;
  bool busy=false, busy_pending=false, more_data=false, fail_encode=false, sync_timeout=false;
  bool bad_timestamp=false, locked_output=false, oversize_output=false;
  bool fail_encoder_close=false, fail_session_close=false, fail_va_close=false, fail_fd_close=false, fail_description_release=false;
  mfxVideoParam parameters{}; mfxExtCodingOption options{}; mfxExtCodingOption2 options2{};
  mfxFrameSurface1* surface=nullptr; mfxBitstream* bitstream=nullptr;
  mfxEncodeCtrl* control=nullptr; mfxHDL display=nullptr;
  State() {
    char* paths[]={node0,node1,node2};
    for(int i=0;i<3;++i) {
      buses[i].domain=0;buses[i].bus=(i==0?3:i==1?0:1);buses[i].dev=2;
      ids[i].vendor_id=i==1?0x1002:0x8086;ids[i].device_id=0x1234+i;
      nodes[i][DRM_NODE_RENDER]=paths[i];devices[i].nodes=nodes[i].data();
      devices[i].available_nodes=1<<DRM_NODE_RENDER;devices[i].bustype=DRM_BUS_PCI;
      devices[i].businfo.pci=&buses[i];devices[i].deviceinfo.pci=&ids[i];
      descriptions[i].Impl=i==1?MFX_IMPL_TYPE_SOFTWARE:MFX_IMPL_TYPE_HARDWARE;
      descriptions[i].VendorID=0x8086;descriptions[i].AccelerationMode=MFX_ACCEL_MODE_VIA_VAAPI;
      extended[i].VendorID=0x8086;extended[i].DeviceID=ids[i].device_id;
      extended[i].PCIBus=buses[i].bus;extended[i].PCIDevice=buses[i].dev;
    }
  }
  void Event(const char* event) {
    if(!owner_set){owner=pthread_self();owner_set=true;}
    Require(pthread_equal(owner,pthread_self()));events.emplace_back(event);
  }
};
State* state=nullptr;
mfxLoader MFX_CDECL Load(){state->Event("load");state->loader=true;return reinterpret_cast<mfxLoader>(1);}
void MFX_CDECL Unload(mfxLoader){state->Event("unload");Require(!state->session);state->loader=false;}
mfxConfig MFX_CDECL ConfigCreate(mfxLoader){state->Event("config");return reinterpret_cast<mfxConfig>(1);}
mfxStatus MFX_CDECL Filter(mfxConfig,const mfxU8* key,mfxVariant value){
  state->Event("filter");Require(value.Type==MFX_VARIANT_TYPE_U32);
  state->filters[reinterpret_cast<const char*>(key)]=value.Data.U32;return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL Enumerate(mfxLoader,mfxU32 index,mfxImplCapsDeliveryFormat format,mfxHDL* result){
  state->Event("enumerate");if(index>=3)return MFX_ERR_NOT_FOUND;
  if(format==MFX_IMPLCAPS_IMPLDESCSTRUCTURE){
    if(state->software_only)state->descriptions[index].Impl=MFX_IMPL_TYPE_SOFTWARE;
    *result=&state->descriptions[index];return MFX_ERR_NONE;
  }
  Require(format==MFX_IMPLCAPS_DEVICE_ID_EXTENDED);
  if(state->no_extended)return MFX_ERR_UNSUPPORTED;
  *result=&state->extended[index];return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL ReleaseDescription(mfxLoader,mfxHDL){state->Event("release_description");return state->fail_description_release?MFX_ERR_UNKNOWN:MFX_ERR_NONE;}
mfxStatus MFX_CDECL SessionCreate(mfxLoader,mfxU32 index,mfxSession* result){
  state->Event("create_session");Require(!state->session && index==static_cast<mfxU32>(state->active_device));
  Require(state->filters["mfxImplDescription.Impl"]==MFX_IMPL_TYPE_HARDWARE);
  Require(state->filters["mfxImplDescription.VendorID"]==0x8086);
  Require(state->filters["mfxImplDescription.AccelerationMode"]==MFX_ACCEL_MODE_VIA_VAAPI);
  state->selected_impl=index;state->session=true;*result=reinterpret_cast<mfxSession>(1);return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL SessionClose(mfxSession){
  state->Event("close_session");if(state->fail_session_close)return MFX_ERR_UNKNOWN;
  state->session=false;state->initialized=false;
  if(state->surface)state->surface->Data.Locked=0;
  for(auto* surface:state->locked_surfaces)surface->Data.Locked=0;
  state->locked_surfaces.clear();
  state->surface=nullptr;state->bitstream=nullptr;state->control=nullptr;return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL QueryImpl(mfxSession,mfxIMPL* impl){state->Event("query_impl");*impl=(state->wrong_runtime?MFX_IMPL_SOFTWARE:MFX_IMPL_HARDWARE)|MFX_IMPL_VIA_VAAPI;return MFX_ERR_NONE;}
mfxStatus MFX_CDECL QueryVersion(mfxSession,mfxVersion* version){state->Event("query_version");version->Major=2;version->Minor=9;return MFX_ERR_NONE;}
mfxStatus MFX_CDECL SetHandle(mfxSession,mfxHandleType type,mfxHDL handle){state->Event("set_handle");Require(type==MFX_HANDLE_VA_DISPLAY && handle==state->display);return MFX_ERR_NONE;}
mfxStatus MFX_CDECL GetHandle(mfxSession,mfxHandleType type,mfxHDL* handle){state->Event("get_handle");Require(type==MFX_HANDLE_VA_DISPLAY);*handle=state->wrong_display?nullptr:state->display;return MFX_ERR_NONE;}
void Save(mfxVideoParam* parameters){
  state->parameters=*parameters;
  for(int i=0;i<parameters->NumExtParam;++i){
    auto* ext=parameters->ExtParam[i];
    if(ext->BufferId==MFX_EXTBUFF_CODING_OPTION)state->options=*reinterpret_cast<mfxExtCodingOption*>(ext);
    if(ext->BufferId==MFX_EXTBUFF_CODING_OPTION2)state->options2=*reinterpret_cast<mfxExtCodingOption2*>(ext);
  }
}
mfxStatus MFX_CDECL Query(mfxSession,mfxVideoParam* input,mfxVideoParam* output){
  state->Event("query");Require(input==output);
  Require(input->AsyncDepth==1 && input->IOPattern==MFX_IOPATTERN_IN_SYSTEM_MEMORY);
  Require(input->mfx.GopRefDist==1 && input->mfx.CodecId==MFX_CODEC_AVC);
  if(state->wrong_profile)output->mfx.CodecProfile=MFX_PROFILE_AVC_HIGH;
  return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL QuerySurfaces(mfxSession,mfxVideoParam* parameters,mfxFrameAllocRequest* request){
  state->Event("query_surfaces");++state->pool_queries;
  request->Type=MFX_MEMTYPE_SYSTEM_MEMORY;request->NumFrameMin=state->pool_min;
  request->NumFrameSuggested=state->pool_suggested;request->Info=parameters->mfx.FrameInfo;
  return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL Initialize(mfxSession,mfxVideoParam* parameters){
  state->Event("initialize");
  if(state->open_gate){
    std::unique_lock<std::mutex> lock(state->open_gate->mutex);
    state->open_gate->entered=true;state->open_gate->cv.notify_all();
    state->open_gate->cv.wait(lock,[]{return state->open_gate->released;});
  }
  if(state->fail_initialize)return MFX_ERR_UNSUPPORTED;
  Save(parameters);state->initialized=true;return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL Reset(mfxSession,mfxVideoParam* parameters){
  state->Event("reset");++state->reset_count;
  Require(!state->surface);if(state->fail_reset)return MFX_ERR_INCOMPATIBLE_VIDEO_PARAM;
  Save(parameters);
  if(state->reset_locks_surface){
    Require(!state->submitted_surfaces.empty());
    auto* surface=state->submitted_surfaces.back();surface->Data.Locked=1;
    state->locked_surfaces.push_back(surface);
  }
  return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL GetParameters(mfxSession,mfxVideoParam* output){
  state->Event("get_parameters");auto** extensions=output->ExtParam;const auto count=output->NumExtParam;
  *output=state->parameters;output->ExtParam=extensions;output->NumExtParam=count;
  for(int i=0;i<count;++i){auto* ext=extensions[i];
    if(ext->BufferId==MFX_EXTBUFF_CODING_OPTION)*reinterpret_cast<mfxExtCodingOption*>(ext)=state->options;
    else if(ext->BufferId==MFX_EXTBUFF_CODING_OPTION2)*reinterpret_cast<mfxExtCodingOption2*>(ext)=state->options2;
    else if(ext->BufferId==MFX_EXTBUFF_CODING_OPTION_SPSPPS){
      auto& headers=*reinterpret_cast<mfxExtCodingOptionSPSPPS*>(ext);++state->header_count;
      const uint8_t sps[]={0x67,0x42,0x00,0x1f,static_cast<uint8_t>(state->header_count)};
      const uint8_t pps[]={0x68,0xce,0x0f};Require(headers.SPSBufSize>=sizeof(sps) && headers.PPSBufSize>=sizeof(pps));
      std::memcpy(headers.SPSBuffer,sps,sizeof(sps));std::memcpy(headers.PPSBuffer,pps,sizeof(pps));
      headers.SPSBufSize=state->malformed_headers?0:sizeof(sps);headers.PPSBufSize=sizeof(pps);
    }else std::abort();
  }
  return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL Encode(mfxSession,mfxEncodeCtrl* control,mfxFrameSurface1* surface,mfxBitstream* bitstream,mfxSyncPoint* sync){
  state->Event("encode");++state->encode_count;
  Require(!state->surface && surface->Data.Pitch%32==0 && !surface->Data.Locked);
  state->submitted_surfaces.push_back(surface);
  Require(reinterpret_cast<uintptr_t>(surface->Data.Y)%32==0 && reinterpret_cast<uintptr_t>(bitstream->Data)%32==0);
  if(state->busy && !state->busy_pending)return MFX_WRN_DEVICE_BUSY;
  state->surface=surface;state->bitstream=bitstream;state->control=control;surface->Data.Locked=1;
  if(state->more_data)return MFX_ERR_MORE_DATA;
  *sync=reinterpret_cast<mfxSyncPoint>(1);
  if(state->busy_pending)return MFX_WRN_DEVICE_BUSY;
  return state->fail_encode?MFX_ERR_DEVICE_FAILED:MFX_ERR_NONE;
}
mfxStatus MFX_CDECL Sync(mfxSession,mfxSyncPoint,mfxU32 wait){
  state->Event("sync");++state->sync_count;Require(wait>0 && wait<=200);
  if(state->sync_timeout)return MFX_WRN_IN_EXECUTION;
  if(!state->surface)return MFX_ERR_NONE;
  const bool key=(state->control->FrameType&MFX_FRAMETYPE_IDR)!=0;
  auto& stream=*state->bitstream;
  const uint8_t data[]={0,0,0,1,static_cast<uint8_t>(key?0x65:0x41),0x80,0x01};
  Require(stream.MaxLength>=sizeof(data));std::memcpy(stream.Data,data,sizeof(data));
  stream.DataOffset=0;stream.DataLength=state->oversize_output?stream.MaxLength+1:sizeof(data);
  stream.TimeStamp=state->surface->Data.TimeStamp+(state->bad_timestamp?1:0);
  stream.FrameType=key?MFX_FRAMETYPE_IDR:MFX_FRAMETYPE_P;
  Require(state->surface->Data.Y[0]==state->expected_y && state->surface->Data.UV[0]==0x85);
  state->surface->Data.Locked=state->locked_output?1:0;
  if(state->locked_output)state->locked_surfaces.push_back(state->surface);
  state->surface=nullptr;state->bitstream=nullptr;state->control=nullptr;return MFX_ERR_NONE;
}
mfxStatus MFX_CDECL EncoderClose(mfxSession){state->Event("close_encoder");if(state->fail_encoder_close)return MFX_ERR_UNKNOWN;return state->initialized?MFX_ERR_NONE:MFX_ERR_NOT_INITIALIZED;}
VADisplay Display(int fd){state->Event("display");state->display=reinterpret_cast<void*>(100+fd);return state->display;}
VAStatus VaInitialize(VADisplay,int* major,int* minor){state->Event("va_initialize");state->va_active=true;*major=1;*minor=20;return VA_STATUS_SUCCESS;}
VAStatus VaTerminate(VADisplay){state->Event("va_terminate");Require(!state->session && !state->loader);if(state->fail_va_close)return VA_STATUS_ERROR_OPERATION_FAILED;state->va_active=false;return VA_STATUS_SUCCESS;}
int Devices(uint32_t,drmDevicePtr* devices,int maximum){state->Event("devices");Require(maximum>=3);for(int i=0;i<state->device_count;++i)devices[i]=&state->devices[i];return state->device_count;}
void FreeDevices(drmDevicePtr*,int){state->Event("free_devices");}
int Device(int fd,uint32_t,drmDevicePtr* result){state->Event("device");Require(fd==10+state->active_device);*result=&state->devices[state->bad_identity?1:state->active_device];return 0;}
void FreeDevice(drmDevicePtr* device){state->Event("free_device");*device=nullptr;}
int OpenNode(const char* path){
  state->Event("open_node");++state->open_count;state->attempted.emplace_back(path);
  for(int i=0;i<3;++i)if(state->nodes[i][DRM_NODE_RENDER] && state->attempted.back()==state->nodes[i][DRM_NODE_RENDER]){
    if(i==state->fail_open_index){errno=state->fail_open_errno;return -1;}
    state->active_device=i;state->fd_open=true;return 10+i;
  }
  std::abort();
}
int CloseNode(int){state->Event("close_node");Require(!state->va_active && !state->session);++state->close_count;if(state->fail_fd_close)return -1;state->fd_open=false;return 0;}
std::shared_ptr<internal::LinuxQsvDriver> Driver(){
  auto d=std::make_shared<internal::LinuxQsvDriver>();
  d->load=Load;d->unload=Unload;d->create_config=ConfigCreate;d->filter=Filter;
  d->enumerate=Enumerate;d->release_description=ReleaseDescription;d->create_session=SessionCreate;
  d->close_session=SessionClose;d->query_impl=QueryImpl;d->query_version=QueryVersion;
  d->set_handle=SetHandle;d->get_handle=GetHandle;d->query=Query;d->query_surfaces=QuerySurfaces;
  d->initialize=Initialize;d->reset=Reset;d->get_parameters=GetParameters;d->encode=Encode;
  d->sync=Sync;d->close_encoder=EncoderClose;d->display=Display;d->initialize_va=VaInitialize;
  d->terminate_va=VaTerminate;d->get_devices=Devices;d->free_devices=FreeDevices;
  d->get_device=Device;d->free_device=FreeDevice;d->open_node=OpenNode;d->close_node=CloseNode;
  Require(d->HasRequiredFunctions());return d;
}
std::unique_ptr<VideoEncoderBackend> Backend(int ordinal=-1){return internal::CreateLinuxQsvBackendForTesting(Driver(),ordinal);}
Config Configuration(){Config c;c.width=64;c.height=48;c.bitrate_bps=200000;c.framerate_fps=30;return c;}
struct Input {
  std::vector<uint8_t> y=std::vector<uint8_t>(96*48,0x28),uv=std::vector<uint8_t>(80*24,0x85);
  Frame frame{y.data(),uv.data(),96,80,64,48,12345};
};
void Empty(){Require(!state->session && !state->loader && !state->fd_open && !state->va_active);}
size_t Index(const char* event){const auto found=std::find(state->events.begin(),state->events.end(),event);Require(found!=state->events.end());return found-state->events.begin();}
void Healthy(){Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kQsv)==Status::kOk);}
LinuxEncoderPoolSnapshot QsvPoolMetrics(){
  return SnapshotLinuxHardwareEncoderMetrics().pools[static_cast<size_t>(LinuxEncoderMetricBackend::kQsv)];
}
void NoOwnedPoolMetrics(){
  const auto pool=QsvPoolMetrics();
  Require(pool.owned_pools==0 && pool.owned_surfaces==0 && pool.owned_bytes==0 &&
      pool.retained_pools==0 && pool.retained_surfaces==0 && pool.retained_bytes==0);
}
void Happy(){
  auto backend=Backend();Require(backend->Open(Configuration())==Status::kOk);
  Require(state->attempted==std::vector<std::string>{"/dev/dri/renderD130"} && state->selected_impl==2);
  const auto info=backend->GetInfo();Require(info.implementation_name=="QSV-VAAPI-Linux" && info.is_hardware_accelerated);
  Input input;Packet first,second;
  Require(backend->Encode(input.frame,true,&first)==Status::kOk && first.frame_id==12345 && first.key_frame);
  input.frame.id=12346;Require(backend->Encode(input.frame,false,&second)==Status::kOk && !second.key_frame);
  const auto saved=first.bytes;second.bytes[0]=7;Require(first.bytes==saved);
  Require(first.bytes.size()>second.bytes.size());
  Require(backend->SetRates({150000,15})==Status::kOk && state->reset_count==1 && state->header_count==2);
  Require(state->parameters.mfx.TargetKbps==150 && state->parameters.mfx.FrameInfo.FrameRateExtN==15);
  input.frame.id=12347;Require(backend->Encode(input.frame,false,&second)==Status::kOk && second.key_frame);
  backend->Close();Require(backend->LastCloseStatus()==Status::kOk);Empty();Healthy();
  Require(Index("close_encoder")<Index("close_session") && Index("close_session")<Index("unload") &&
      Index("unload")<Index("va_terminate") && Index("va_terminate")<Index("close_node"));
  const auto count=state->events.size();backend->Close();Require(count==state->events.size());
  const auto metrics=SnapshotLinuxHardwareEncoderMetrics();
  const auto& copy=metrics.transfers[static_cast<size_t>(LinuxEncoderMetricTransfer::kQsvCpuSurfaceCopy)];
  Require(copy.measurement_available && copy.started_calls==3 && copy.successful_calls==3 &&
      copy.requested_bytes==13824 && copy.successful_bytes==13824 && copy.completed.samples==3);
  const auto pool=QsvPoolMetrics();
  Require(pool.allocation_calls==1 && pool.allocated_surfaces==1 && pool.rebuild_started==0);
  NoOwnedPoolMetrics();Require(!metrics.gpu_readback_bytes.has_value());
}
void Explicit(){auto b=Backend(1);Require(b->Open(Configuration())==Status::kOk && state->selected_impl==0);Require(state->attempted==std::vector<std::string>{"/dev/dri/renderD129"});b->Close();Empty();}
void StableFallback(){state->fail_open_index=2;auto b=Backend();Require(b->Open(Configuration())==Status::kOk);Require(state->attempted==std::vector<std::string>({"/dev/dri/renderD130","/dev/dri/renderD129"}));b->Close();Empty();}
void ExplicitNoFallback(){state->fail_open_index=2;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnavailable && state->open_count==1);Empty();}
void OpenPermissionReason(){
  state->fail_open_index=2;
  for(const int error:{EACCES,EPERM,ENOENT}){
    state->fail_open_errno=error;auto b=Backend(0);
    Require(b->Open(Configuration())==Status::kUnavailable);
    Require(b->LastError()==(error==ENOENT?"qsv_render_node_open_failed":"qsv_render_node_permission_denied"));
    Empty();Healthy();
  }
}
void NoDevice(){state->device_count=0;auto b=Backend();Require(b->Open(Configuration())==Status::kUnavailable && state->open_count==0);Empty();}
void MissingOrdinal(){auto b=Backend(2);Require(b->Open(Configuration())==Status::kUnavailable && state->open_count==0);Empty();}
void NonIntel(){for(auto& d:state->ids)d.vendor_id=0x1002;auto b=Backend();Require(b->Open(Configuration())==Status::kUnavailable && state->open_count==0);Empty();}
void MissingFunction(){auto d=Driver();d->sync=nullptr;auto b=internal::CreateLinuxQsvBackendForTesting(d);Require(b->Open(Configuration())==Status::kUnavailable && state->events.empty());Empty();}
void InvalidConfiguration(){auto b=Backend();auto c=Configuration();c.width=63;Require(b->Open(c)==Status::kInvalidArgument);c=Configuration();c.codec=Codec::kH265;Require(b->Open(c)==Status::kUnsupported);c=Configuration();c.h264_level=0;Require(b->Open(c)==Status::kInvalidArgument && state->events.empty());Empty();}
void BadIdentity(){state->bad_identity=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnsupported && !state->session);Empty();Healthy();}
void NoExtended(){state->no_extended=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnsupported);Empty();Healthy();}
void SoftwareOnly(){state->software_only=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnavailable);Empty();Healthy();}
void WrongRuntime(){state->wrong_runtime=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnsupported);Empty();Healthy();}
void WrongDisplay(){state->wrong_display=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnsupported);Empty();Healthy();}
void InitFailure(){state->fail_initialize=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnsupported);Empty();Healthy();}
void WrongProfile(){state->wrong_profile=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnsupported);Empty();Healthy();}
void MultiSurfacePool(){
  state->pool_min=2;state->pool_suggested=3;
  auto b=Backend(0);Require(b->Open(Configuration())==Status::kOk);
  auto pool=QsvPoolMetrics();
  Require(pool.allocation_calls==1 && pool.owned_pools==1 && pool.owned_surfaces==3 && pool.owned_bytes==13824);
  Input input;Packet packet;
  for(int n=0;n<6;++n){input.frame.id=12345+n;Require(b->Encode(input.frame,n==0,&packet)==Status::kOk && packet.frame_id==input.frame.id);}
  Require(state->submitted_surfaces.size()==6);
  Require(state->submitted_surfaces[0]!=state->submitted_surfaces[1] &&
      state->submitted_surfaces[1]!=state->submitted_surfaces[2] &&
      state->submitted_surfaces[0]!=state->submitted_surfaces[2]);
  for(int n=0;n<3;++n)Require(state->submitted_surfaces[n]==state->submitted_surfaces[n+3]);
  const auto queries=state->pool_queries;
  state->pool_min=4;state->pool_suggested=5;
  Require(b->SetRates({150000,15})==Status::kOk && state->pool_queries>queries);
  pool=QsvPoolMetrics();
  Require(pool.allocation_calls==2 && pool.allocated_surfaces==8 && pool.owned_pools==1 &&
      pool.owned_surfaces==5 && pool.owned_bytes==23040 && pool.retained_pools==0);
  state->submitted_surfaces.clear();
  for(int n=0;n<5;++n){input.frame.id=12400+n;Require(b->Encode(input.frame,false,&packet)==Status::kOk);if(n==0)Require(packet.key_frame);}
  auto unique=state->submitted_surfaces;std::sort(unique.begin(),unique.end());
  unique.erase(std::unique(unique.begin(),unique.end()),unique.end());Require(unique.size()==5);
  b->Close();Empty();Healthy();NoOwnedPoolMetrics();
  const auto metrics=SnapshotLinuxHardwareEncoderMetrics();
  const auto& copy=metrics.transfers[static_cast<size_t>(LinuxEncoderMetricTransfer::kQsvCpuSurfaceCopy)];
  Require(copy.successful_calls==11 && copy.successful_bytes==50688 && copy.completed.samples==11);
}
void PoolLimit(){
  state->pool_min=17;state->pool_suggested=17;auto b=Backend(0);
  Require(b->Open(Configuration())==Status::kUnsupported && state->encode_count==0);
  Require(std::find(state->events.begin(),state->events.end(),"initialize")==state->events.end());Empty();Healthy();
}
void PoolMemoryLimit(){
  state->pool_min=16;state->pool_suggested=16;auto b=Backend(0);auto config=Configuration();config.width=8192;config.height=8192;
  Require(b->Open(config)==Status::kUnsupported && state->encode_count==0);Empty();Healthy();
}
void ResetPoolLimit(){
  auto b=Backend(0);Require(b->Open(Configuration())==Status::kOk);
  state->pool_min=17;state->pool_suggested=17;
  Require(b->SetRates({150000,15})==Status::kUnsupported && state->reset_count==1);Empty();Healthy();
}
void MalformedHeaders(){state->malformed_headers=true;auto b=Backend(0);Require(b->Open(Configuration())==Status::kUnsupported);Empty();Healthy();}
void ResetFailure(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->fail_reset=true;Require(b->SetRates({100000,15})==Status::kUnsupported);Empty();Healthy();}
void EncodeFailure(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->fail_encode=true;Input i;Packet p;Require(b->Encode(i.frame,true,&p)==Status::kError && p.bytes.empty());Empty();Healthy();}
void Busy(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->busy=true;Input i;Packet p;Require(b->Encode(i.frame,true,&p)==Status::kUnavailable && p.bytes.empty());Require(state->encode_count==1 && state->sync_count==0);Empty();Healthy();}
void BusyPending(){state->busy_pending=true;EncodeFailure();Require(state->encode_count==1);}
void MoreData(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->more_data=true;Input i;Packet p;Require(b->Encode(i.frame,true,&p)==Status::kUnsupported && p.bytes.empty());Empty();Healthy();Require(state->encode_count==1);}
void InvalidOutput(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);Input i;Packet p;Require(b->Encode(i.frame,true,&p)==Status::kError && p.bytes.empty());Empty();Healthy();}
void Timestamp(){state->bad_timestamp=true;InvalidOutput();}
void Locked(){
  state->pool_min=2;state->pool_suggested=3;
  auto b=Backend(0);Require(b->Open(Configuration())==Status::kOk);
  Input input;Packet packet;state->locked_output=true;
  Require(b->Encode(input.frame,true,&packet)==Status::kOk);
  auto* first=state->submitted_surfaces.front();Require(first->Data.Locked && first->Data.Y[0]==0x28);
  state->locked_output=false;state->expected_y=0x39;std::fill(input.y.begin(),input.y.end(),0x39);
  for(int n=0;n<4;++n){input.frame.id=12500+n;Require(b->Encode(input.frame,false,&packet)==Status::kOk);}
  for(size_t n=1;n<state->submitted_surfaces.size();++n)Require(state->submitted_surfaces[n]!=first);
  Require(first->Data.Locked && first->Data.Y[0]==0x28);
  b->Close();Empty();Healthy();
}
void LockedPoolRetained(){
  state->pool_min=2;state->pool_suggested=2;state->locked_output=true;
  auto driver=Driver();std::weak_ptr<internal::LinuxQsvDriver> weak=driver;
  auto b=internal::CreateLinuxQsvBackendForTesting(driver,0);driver.reset();
  Require(b->Open(Configuration())==Status::kOk);Input input;Packet packet;
  Require(b->Encode(input.frame,true,&packet)==Status::kOk);++input.frame.id;
  Require(b->Encode(input.frame,false,&packet)==Status::kOk);
  Require(state->locked_surfaces.size()==2 && state->submitted_surfaces[0]!=state->submitted_surfaces[1]);
  state->fail_session_close=true;++input.frame.id;
  Require(b->Encode(input.frame,false,&packet)==Status::kRetirementFailed && packet.bytes.empty());
  Require(state->encode_count==2 && b->LastCloseStatus()==Status::kRetirementFailed);
  const auto count=state->events.size();b.reset();Require(!weak.expired() && state->events.size()==count);
  for(auto* surface:state->locked_surfaces)Require(surface->Data.Locked && surface->Data.Y[0]==0x28 && surface->Data.UV[0]==0x85);
  const auto pool=QsvPoolMetrics();
  Require(pool.owned_pools==1 && pool.retained_pools==1 && pool.owned_surfaces==2 &&
      pool.retained_surfaces==2 && pool.owned_bytes==9216 && pool.retained_bytes==9216);
}
void ResetWithLockedPool(){
  state->pool_min=2;state->pool_suggested=3;state->locked_output=true;
  auto b=Backend(0);Require(b->Open(Configuration())==Status::kOk);Input input;Packet packet;
  Require(b->Encode(input.frame,true,&packet)==Status::kOk);
  const auto original=state->attempted.front();
  Require(b->SetRates({150000,15})==Status::kOk && state->reset_count==0);
  Require(state->attempted.size()==2 && state->attempted.back()==original);
  Require(state->parameters.mfx.TargetKbps==150 && state->parameters.mfx.FrameInfo.FrameRateExtN==15);
  ++input.frame.id;state->locked_output=false;
  Require(b->Encode(input.frame,false,&packet)==Status::kOk && packet.key_frame && packet.frame_id==input.frame.id);
  auto pool=QsvPoolMetrics();
  Require(pool.rebuild_started==1 && pool.rebuild_succeeded==1 && pool.rebuild_failed==0 &&
      pool.allocation_calls==2 && pool.owned_pools==1 && pool.owned_surfaces==3 && pool.owned_bytes==13824);
  b->Close();Empty();Healthy();NoOwnedPoolMetrics();
}
void ResetPostLockedPool(){
  state->pool_min=2;state->pool_suggested=3;
  auto b=Backend(0);Require(b->Open(Configuration())==Status::kOk);Input input;Packet packet;
  Require(b->Encode(input.frame,true,&packet)==Status::kOk);state->reset_locks_surface=true;
  Require(b->SetRates({150000,15})==Status::kOk && state->reset_count==1 && state->attempted.size()==2);
  ++input.frame.id;Require(b->Encode(input.frame,false,&packet)==Status::kOk && packet.key_frame);
  b->Close();Empty();Healthy();
}
void RateReopenCloseFailureRetained(){
  state->pool_min=2;state->pool_suggested=3;state->locked_output=true;
  auto driver=Driver();std::weak_ptr<internal::LinuxQsvDriver> weak=driver;
  auto b=internal::CreateLinuxQsvBackendForTesting(driver,0);driver.reset();
  Require(b->Open(Configuration())==Status::kOk);Input input;Packet packet;
  Require(b->Encode(input.frame,true,&packet)==Status::kOk);
  auto* retained=state->locked_surfaces.front();state->fail_session_close=true;
  Require(b->SetRates({150000,15})==Status::kRetirementFailed && state->attempted.size()==1);
  const auto count=state->events.size();b.reset();
  Require(!weak.expired() && state->events.size()==count && retained->Data.Locked && retained->Data.Y[0]==0x28);
  const auto pool=QsvPoolMetrics();
  Require(pool.rebuild_started==1 && pool.rebuild_succeeded==0 && pool.rebuild_failed==1 &&
      pool.owned_pools==1 && pool.owned_surfaces==3 && pool.owned_bytes==13824 &&
      pool.retained_pools==1 && pool.retained_surfaces==3 && pool.retained_bytes==13824);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kQsv)==Status::kRetirementFailed);
}
void RateReopenInitFailureSticky(){
  state->pool_min=2;state->pool_suggested=3;state->locked_output=true;
  auto b=Backend(0);Require(b->Open(Configuration())==Status::kOk);Input input;Packet packet;
  Require(b->Encode(input.frame,true,&packet)==Status::kOk);state->fail_initialize=true;
  Require(b->SetRates({150000,15})==Status::kRetirementFailed && state->attempted.size()==2);
  Require(b->LastCloseStatus()==Status::kRetirementFailed);
  Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kQsv)==Status::kRetirementFailed);Empty();
  const auto pool=QsvPoolMetrics();
  Require(pool.rebuild_started==1 && pool.rebuild_failed==1 && pool.rebuild_succeeded==0);
  NoOwnedPoolMetrics();
}
void Oversize(){state->oversize_output=true;InvalidOutput();}
void SyncTimeout(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->sync_timeout=true;Input i;Packet p;Require(b->Encode(i.frame,true,&p)==Status::kRetirementFailed && p.bytes.empty());Require(b->LastCloseStatus()==Status::kRetirementFailed);Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kQsv)==Status::kRetirementFailed);Empty();}
void Retained(){
  auto d=Driver();std::weak_ptr<internal::LinuxQsvDriver> weak=d;
  auto b=internal::CreateLinuxQsvBackendForTesting(d);d.reset();Require(b->Open(Configuration())==Status::kOk);
  state->fail_session_close=true;b->Close();Require(b->LastCloseStatus()==Status::kRetirementFailed);
  Require(state->session && state->loader && state->va_active && state->fd_open);
  const auto count=state->events.size();b->Close();b.reset();Require(state->events.size()==count && !weak.expired());
  auto next=Backend();Require(next->Open(Configuration())==Status::kRetirementFailed && state->events.size()==count);
}
void EncoderCloseFailure(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->fail_encoder_close=true;b->Close();Require(b->LastCloseStatus()==Status::kRetirementFailed);Empty();Require(HardwareEncoderProbes().Check(HardwareEncoderVendor::kQsv)==Status::kRetirementFailed);NoOwnedPoolMetrics();}
void VaCloseFailure(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->fail_va_close=true;b->Close();Require(b->LastCloseStatus()==Status::kRetirementFailed && state->fd_open && state->va_active && !state->session);}
void FdCloseFailure(){auto b=Backend();Require(b->Open(Configuration())==Status::kOk);state->fail_fd_close=true;b->Close();Require(b->LastCloseStatus()==Status::kRetirementFailed && state->fd_open && !state->va_active);}
void DescriptionReleaseFailure(){state->fail_description_release=true;auto b=Backend();Require(b->Open(Configuration())==Status::kRetirementFailed);Require(b->LastCloseStatus()==Status::kRetirementFailed && state->loader && state->fd_open && state->va_active);}
void BoundedOwner(){
  auto raw=Backend();auto b=CreateLinuxBoundedBackend(std::move(raw),HardwareEncoderVendor::kQsv,{"QSV-VAAPI-Linux",true});
  Require(b->Open(Configuration())==Status::kOk);Input i;Packet p;Require(b->Encode(i.frame,true,&p)==Status::kOk);
  Require(b->SetRates({100000,15})==Status::kOk);b->Close();Require(b->LastCloseStatus()==Status::kOk);b.reset();
  for(int n=0;internal::LinuxBoundedWorkerCountForTesting()!=0;++n){Require(n<2000);usleep(1000);}
  Require(!pthread_equal(state->owner,pthread_self()));Empty();Healthy();
}
void ResolvedSelection(){
  auto collector=std::make_shared<internal::LinuxQsvDeviceCollector>();
  auto b=internal::CreateLinuxQsvBackendForTesting(Driver(),-1,collector);
  Require(b->Open(Configuration())==Status::kOk && !collector->RetiredSelection());
  b->Close();auto selected=collector->RetiredSelection();
  Require(selected && selected->render_node=="/dev/dri/renderD130" && selected->vendor_id==0x8086 &&
      selected->device_id==0x1236 && selected->pci_domain==0 && selected->pci_bus==1 &&
      selected->pci_device==2 && selected->pci_function==0 && selected->vpl_api_major==2 && selected->vpl_api_minor==9 && selected->va_api_major==1 && selected->va_api_minor==20);
  const auto enumerations=std::count(state->events.begin(),state->events.end(),"devices");
  state->device_count=0;
  auto pinned=internal::CreateLinuxQsvBackendForTesting(Driver(),*selected);
  Require(pinned->Open(Configuration())==Status::kOk && state->selected_impl==2);
  Require(std::count(state->events.begin(),state->events.end(),"devices")==enumerations);
  pinned->Close();Empty();Healthy();
}
void ResolvedPciDrift(){
  auto collector=std::make_shared<internal::LinuxQsvDeviceCollector>();
  auto b=internal::CreateLinuxQsvBackendForTesting(Driver(),-1,collector);
  Require(b->Open(Configuration())==Status::kOk);b->Close();auto selected=collector->RetiredSelection();Require(selected.has_value());
  state->buses[2].bus=9;const auto sessions=std::count(state->events.begin(),state->events.end(),"create_session");
  auto pinned=internal::CreateLinuxQsvBackendForTesting(Driver(),*selected);
  Require(pinned->Open(Configuration())==Status::kUnsupported && state->open_count==2);
  Require(std::count(state->events.begin(),state->events.end(),"create_session")==sessions);Empty();Healthy();
}
void ResolvedNoFallback(){
  state->fail_open_index=2;
  const LinuxQsvDeviceSelection selected{"/dev/dri/renderD130",0x8086,0x1236,0,1,2,0};
  auto pinned=internal::CreateLinuxQsvBackendForTesting(Driver(),selected);
  Require(pinned->Open(Configuration())==Status::kUnavailable && state->open_count==1);Empty();Healthy();
}
void ResolvedCloseFailure(){
  auto collector=std::make_shared<internal::LinuxQsvDeviceCollector>();
  auto b=internal::CreateLinuxQsvBackendForTesting(Driver(),-1,collector);
  Require(b->Open(Configuration())==Status::kOk);state->fail_session_close=true;b->Close();
  Require(b->LastCloseStatus()==Status::kRetirementFailed && !collector->RetiredSelection());
}
void ResolvedInvalid(){
  const LinuxQsvDeviceSelection selected{"/dev/dri/renderD130",0x1002,0x1236,0,1,2,0};
  auto pinned=internal::CreateLinuxQsvBackendForTesting(Driver(),selected);
  Require(pinned->Open(Configuration())==Status::kInvalidArgument && state->events.empty());Empty();
}
void LateCollectorOwned(){
  using namespace std::chrono_literals;
  auto gate=std::make_shared<OpenGate>();state->open_gate=gate;
  auto collector=std::make_shared<internal::LinuxQsvDeviceCollector>();
  std::weak_ptr<internal::LinuxQsvDeviceCollector> weak=collector;
  auto raw=internal::CreateLinuxQsvBackendForTesting(Driver(),-1,collector);
  auto b=CreateLinuxBoundedBackend(std::move(raw),HardwareEncoderVendor::kQsv,
      {"QSV-VAAPI-Linux",true},{40ms,40ms,40ms,40ms});
  Require(b->Open(Configuration())==Status::kRetirementFailed);
  {
    std::unique_lock<std::mutex> lock(gate->mutex);
    Require(gate->cv.wait_for(lock,2s,[&]{return gate->entered;}));
  }
  Require(!collector->RetiredSelection());collector.reset();b.reset();Require(!weak.expired());
  {std::lock_guard<std::mutex> lock(gate->mutex);gate->released=true;gate->cv.notify_all();}
  for(int n=0;internal::LinuxBoundedWorkerCountForTesting()!=0;++n){Require(n<2000);usleep(1000);}
  Require(weak.expired());Empty();
}
void LoaderAbsent(){Status status;std::string reason;auto d=internal::LoadLinuxQsvDriverForTesting("/cohavora-absent-vpl.so","libc.so.6","libc.so.6","libc.so.6",&status,&reason);Require(!d && status==Status::kUnavailable && reason=="qsv_vpl_library_unavailable");}
void LoaderMissingEntry(){Status status;std::string reason;auto d=internal::LoadLinuxQsvDriverForTesting("libc.so.6","libc.so.6","libc.so.6","libc.so.6",&status,&reason);Require(!d && status==Status::kUnavailable && reason=="qsv_driver_entry_point_missing");}
}  // namespace
}  // namespace webrtc::external_encoder
int main(){
  using namespace webrtc::external_encoder;
  struct Test{const char* name;void(*run)();};
  const Test tests[]={
    {"happy_stable_pci_key_delta_owned_rates_headers_release",Happy},{"explicit_device",Explicit},
    {"stable_candidate_fallback",StableFallback},{"explicit_no_device_fallback",ExplicitNoFallback},
    {"node_permission_reason_only_eacces_eperm",OpenPermissionReason},{"no_device",NoDevice},{"missing_ordinal",MissingOrdinal},{"non_intel_ignored",NonIntel},
    {"missing_function",MissingFunction},{"invalid_configuration",InvalidConfiguration},
    {"opened_fd_identity_mismatch",BadIdentity},{"extended_identity_required",NoExtended},
    {"software_implementation_rejected",SoftwareOnly},{"runtime_software_rejected",WrongRuntime},
    {"same_va_display_required",WrongDisplay},{"partial_init_failure_cleanup",InitFailure},
    {"profile_readback_mismatch",WrongProfile},{"multi_surface_pool_rotation_reset_growth",MultiSurfacePool},{"surface_count_limit_before_init",PoolLimit},
    {"surface_pool_memory_limit",PoolMemoryLimit},{"reset_surface_pool_limit",ResetPoolLimit},
    {"malformed_parameter_sets",MalformedHeaders},{"rate_reset_failure_cleanup",ResetFailure},
    {"encode_error_cleanup",EncodeFailure},{"device_busy_one_submission",Busy},
    {"device_busy_pending_cleanup",BusyPending},{"delayed_output_unsupported",MoreData},
    {"timestamp_mismatch",Timestamp},{"locked_surface_skipped_and_not_overwritten",Locked},{"locked_pool_retained_on_cleanup_failure",LockedPoolRetained},
    {"locked_reference_rates_reopen_same_device_idr",ResetWithLockedPool},{"reset_locked_reference_reopen",ResetPostLockedPool},
    {"rate_reopen_close_failure_retains_pool",RateReopenCloseFailureRetained},{"rate_reopen_init_failure_sticky",RateReopenInitFailureSticky},{"output_size_invalid",Oversize},
    {"cooperative_sync_timeout_sticky_poison",SyncTimeout},{"session_close_failure_retained",Retained},
    {"encoder_close_failure_poison",EncoderCloseFailure},{"va_terminate_failure_retained",VaCloseFailure},
    {"fd_close_failure_retained",FdCloseFailure},{"description_release_failure_retained",DescriptionReleaseFailure},
    {"bounded_owner_integration",BoundedOwner},{"resolved_selection_confirmed_close_no_reenumeration",ResolvedSelection},
    {"resolved_path_pci_drift_rejected",ResolvedPciDrift},{"resolved_no_device_fallback",ResolvedNoFallback},
    {"resolved_close_failure_no_output",ResolvedCloseFailure},{"resolved_invalid_no_driver",ResolvedInvalid},
    {"late_collector_owned_through_retirement",LateCollectorOwned},{"loader_absent",LoaderAbsent},{"loader_missing_entry",LoaderMissingEntry}
  };
  int passed=0;
  for(const auto& test:tests){const pid_t child=fork();if(child==0){State local;state=&local;test.run();std::_Exit(0);}
    Require(child>0);int status=0;Require(waitpid(child,&status,0)==child);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){std::cerr<<test.name<<" FAIL\n";return 1;}
    std::cout<<test.name<<" PASS\n";++passed;
  }
  std::cout<<"linux_qsv_backend_fake_tests PASS checks="<<passed<<" hardware=NOT_RUN\n";return 0;
}
