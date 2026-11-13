/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
// Standalone Linux ownership regression; no gtest, GPU, service or devices.
#include "pc/rtp_sender.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "api/video/i420_buffer.h"
#include "call/call.h"
#include "call/call_config.h"
#include "media/base/video_broadcaster.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "media/engine/webrtc_video_engine.h"
#include "pc/video_track.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
namespace webrtc {
namespace {
void Require(bool value, const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
  if (!value) { std::cerr << "requirement failed at " << file << ":" << line << "\n"; std::abort(); }
}
struct State {
  std::atomic<int> created{0}, selector_creations{0}, live_selectors{0};
  std::atomic<bool> factory_alive{true};
};
class Selector final : public VideoEncoderFactory::EncoderSelectorInterface {
 public:
  explicit Selector(std::shared_ptr<State> state) : state_(std::move(state)) {
    ++state_->live_selectors;
  }
  ~Selector() override {
    // Selectors live inside the native stream. A factory destroyed before its
    // stream cannot safely back lazy Create/callbacks on the encoder queue.
    Require(state_->factory_alive); --state_->live_selectors;
  }
  void OnCurrentEncoder(const SdpVideoFormat&) override {}
  std::optional<SdpVideoFormat> OnAvailableBitrate(const DataRate&) override { return std::nullopt; }
  std::optional<SdpVideoFormat> OnEncoderBroken() override { return std::nullopt; }
 private:
  std::shared_ptr<State> state_;
};
class Factory final : public VideoEncoderFactory {
 public:
  explicit Factory(std::shared_ptr<State> state) : state_(std::move(state)) {}
  ~Factory() override {
    Require(state_->live_selectors == 0); state_->factory_alive = false;
  }
  std::vector<SdpVideoFormat> GetSupportedFormats() const override { return {SdpVideoFormat("VP8")}; }
  std::unique_ptr<VideoEncoder> Create(const Environment& env,const SdpVideoFormat& format) override {
    Require(state_->factory_alive); ++state_->created;
    Require(format.name == "VP8");
    return CreateVp8Encoder(env);
  }
  std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override {
    Require(state_->factory_alive); ++state_->selector_creations;
    return std::make_unique<Selector>(state_);
  }
 private:
  std::shared_ptr<State> state_;
};
class Source : public VideoTrackSource {
 public:
  Source() : VideoTrackSource(false) {}
  void Push() {
    auto pixels = I420Buffer::Create(320,240);
    std::memset(pixels->MutableDataY(),64,pixels->StrideY()*240);
    std::memset(pixels->MutableDataU(),96,pixels->StrideU()*120);
    std::memset(pixels->MutableDataV(),160,pixels->StrideV()*120);
    broadcaster_.OnFrame(VideoFrame::Builder().set_video_frame_buffer(pixels)
        .set_timestamp_us(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()).build());
  }
 private:
  VideoSourceInterface<VideoFrame>* source() override { return &broadcaster_; }
  VideoBroadcaster broadcaster_;
};
struct Channel {
  explicit Channel(const Environment& env)
      : call(Call::Create(CallConfig(env))),
        generic_state(std::make_shared<State>()), generic(generic_state),
        allocator(CreateBuiltinVideoBitrateAllocatorFactory()),
        channel(env,call.get(),MediaConfig(),VideoOptions(),CryptoOptions(),&generic,allocator.get()) {
    VideoSenderParameters parameters;
    parameters.codecs = {CreateVideoCodec(96,"VP8")};
    Require(channel.SetSenderParameters(parameters));
    channel.OnReadyToSend(true);
    Require(channel.SetSend(true));
  }
  std::unique_ptr<Call> call;
  std::shared_ptr<State> generic_state;
  Factory generic;
  std::unique_ptr<VideoBitrateAllocatorFactory> allocator;
  WebRtcVideoSendChannel channel;
};
void SenderApiRejectsLateAndAudio() {
  AutoThread main;
  const auto env = CreateEnvironment();
  Channel context(env);
  Require(context.channel.AddSendStream(StreamParams::CreateLegacy(11)));
  auto state = std::make_shared<State>();
  auto factory = std::make_shared<Factory>(state);
  auto sender = VideoRtpSender::Create(env,Thread::Current(),"video",nullptr);
  Require(sender->SetVideoEncoderFactory(factory).ok());
  sender->SetMediaChannel(&context.channel);
  sender->SetSsrc(11);
  Require(sender->SetVideoEncoderFactory(factory).type() == RTCErrorType::INVALID_STATE);
  sender->Stop();
  Require(sender->SetVideoEncoderFactory(factory).type() == RTCErrorType::INVALID_STATE);
  auto audio = AudioRtpSender::Create(env,Thread::Current(),"audio",nullptr,nullptr);
  Require(audio->SetVideoEncoderFactory(factory).type() == RTCErrorType::UNSUPPORTED_OPERATION);
  audio->Stop();
}
void ChannelReplacementBeforeSourceOwnsFactories() {
  AutoThread main;
  const auto env = CreateEnvironment(); Channel context(env);
  auto first_state = std::make_shared<State>(), second_state = std::make_shared<State>();
  auto first = std::make_shared<Factory>(first_state), second = std::make_shared<Factory>(second_state);
  Require(!context.channel.SetVideoEncoderFactory(11,first).ok());
  Require(context.channel.AddSendStream(StreamParams::CreateLegacy(11)));
  Require(context.channel.SetVideoEncoderFactory(11,first).ok());
  Require(first_state->selector_creations >= 1 && first_state->live_selectors == 1);
  std::weak_ptr<Factory> old = first; first.reset();
  Require(!old.expired());
  Require(context.channel.SetVideoEncoderFactory(11,second).ok());
  Require(old.expired() && !first_state->factory_alive && first_state->live_selectors == 0);
  VideoBroadcaster source;
  Require(context.channel.SetVideoSend(11,nullptr,&source));
  Require(context.channel.SetVideoEncoderFactory(11,second).ok());
  auto other = std::make_shared<Factory>(std::make_shared<State>());
  Require(context.channel.SetVideoEncoderFactory(11,other).type() == RTCErrorType::INVALID_STATE);
  std::weak_ptr<Factory> owned = second;second.reset();
  Require(!owned.expired());
  Require(context.channel.SetVideoSend(11,nullptr,nullptr));
  Require(context.channel.RemoveSendStream(11));
  Require(owned.expired() && !second_state->factory_alive && second_state->live_selectors == 0);
}
void SenderBindingDelayedCreateSsrcChangeAndStop() {
  AutoThread main;
  const auto env = CreateEnvironment(); Channel context(env);
  Require(context.channel.AddSendStream(StreamParams::CreateLegacy(11)));
  Require(context.channel.AddSendStream(StreamParams::CreateLegacy(33)));
  const auto state = std::make_shared<State>();
  auto publication = std::make_shared<Factory>(state);
  std::weak_ptr<Factory> owned = publication;
  auto sender = VideoRtpSender::Create(env,Thread::Current(),"publication",nullptr);
  auto source = make_ref_counted<Source>();
  auto track = VideoTrack::Create("synthetic",source,Thread::Current());
  Require(sender->SetVideoEncoderFactory(publication).ok());
  Require(sender->SetTrack(track.get()));
  sender->SetMediaChannel(&context.channel);
  sender->SetSsrc(11);
  Require(state->selector_creations == 1 && state->live_selectors == 1);
  Require(state->created == 0);  // Actual encoder creation is still lazy.
  publication.reset();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (state->created == 0) {
    Require(std::chrono::steady_clock::now() < deadline);
    source->Push();Thread::Current()->ProcessMessages(10);
  }
  Require(context.generic_state->created == 0 && !owned.expired());
  Require(sender->SetVideoEncoderFactory(std::make_shared<Factory>(std::make_shared<State>())).type() == RTCErrorType::INVALID_STATE);
  sender->SetSsrc(33);
  Require(state->live_selectors == 2);  // Old SSRC stream is detached but alive.
  Require(sender->SetTrack(nullptr));
  sender->SetSsrc(0);
  Require(sender->SetVideoEncoderFactory(nullptr).type() == RTCErrorType::INVALID_STATE);
  sender->Stop();sender->SetMediaChannel(nullptr);sender = nullptr;
  Require(!owned.expired());  // Native streams still retain their own lease.
  Require(context.channel.RemoveSendStream(11));Require(!owned.expired());
  Require(context.channel.RemoveSendStream(33));
  Require(owned.expired() && !state->factory_alive && state->live_selectors == 0);
}
}  // namespace
}  // namespace webrtc
int main() {
  using namespace webrtc;
  LogMessage::LogToDebug(LS_NONE);
  struct Test { const char* name;void(*run)(); };
  const Test tests[] = {
      {"signaling_api_rejects_audio_ssrc_and_stopped",SenderApiRejectsLateAndAudio},
      {"worker_stream_factory_replacement_and_native_owner_order",ChannelReplacementBeforeSourceOwnsFactories},
      {"bound_lazy_create_ssrc_change_stop_native_lease",SenderBindingDelayedCreateSsrcChangeAndStop}};
  int passed = 0;
  for(const auto& test:tests) {
    const auto child = fork();
    if(child == 0) { test.run();std::_Exit(0); }
    Require(child > 0);int status = 0;Require(waitpid(child,&status,0) == child);
    if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) { std::cerr<<test.name<<" FAIL status="<<status<<"\n";return 1; }
    std::cout<<test.name<<" PASS\n";++passed;
  }
  std::cout<<"linux_sender_factory_binding_tests PASS checks="<<passed<<" hardware=NOT_RUN\n";
}
