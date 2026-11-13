/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "audio/audio_state.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/audio_device_defines.h"
#include "api/audio/audio_processing.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "api/sequence_checker.h"
#include "api/task_queue/task_queue_base.h"
#include "api/units/time_delta.h"
#include "audio/audio_send_stream.h"
#include "call/audio_receive_stream.h"
#include "call/audio_sender.h"
#include "call/audio_state.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/task_utils/repeating_task.h"

namespace webrtc {
namespace internal {

AudioState::AudioState(const AudioState::Config& config)
    : config_(config),
      audio_transport_(config_.audio_mixer.get(),
                       config_.audio_processing.get(),
                       config_.async_audio_processing_factory.get()) {
  RTC_DCHECK(config_.audio_mixer);
  RTC_DCHECK(config_.audio_device_module);
}

AudioState::~AudioState() {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK(receiving_streams_.empty());
  RTC_DCHECK(sending_streams_.empty());
  RTC_DCHECK(!null_audio_poller_.Running());
}

AudioProcessing* AudioState::audio_processing() {
  return config_.audio_processing.get();
}

AudioTransport* AudioState::audio_transport() {
  return &audio_transport_;
}

void AudioState::SetPlayout(bool enabled) {
  RTC_LOG(LS_INFO) << "SetPlayout(" << enabled << ")";
  RTC_DCHECK_RUN_ON(&thread_checker_);
  auto* adm = config_.audio_device_module.get();
  if (enabled) {
    if (!receiving_streams_.empty()) {
      if (!adm->Playing()) {
        if (adm->InitPlayout() == 0) {
          adm->StartPlayout();
        }
      }
    }
  } else {
    // Disable playout.
    config_.audio_device_module->StopPlayout();
  }
  playout_enabled_ = enabled;
  UpdateNullAudioPollerState();
}

void AudioState::AddReceivingStream(
    webrtc::AudioReceiveStreamInterface* stream) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK_EQ(0, receiving_streams_.count(stream));
  receiving_streams_.insert(stream);
  if (!config_.audio_mixer->AddSource(stream->source())) {
    RTC_DLOG(LS_ERROR) << "Failed to add source to mixer.";
  }

  // Make sure playback is initialized; start playing if enabled.
  if (playout_enabled_) {
    auto* adm = config_.audio_device_module.get();
    if (!adm->Playing()) {
      if (adm->InitPlayout() == 0) {
        adm->StartPlayout();
      } else {
        RTC_DLOG_F(LS_ERROR) << "Failed to initialize playout.";
      }
    }
  }
  UpdateNullAudioPollerState();
}

void AudioState::RemoveReceivingStream(
    webrtc::AudioReceiveStreamInterface* stream) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  auto count = receiving_streams_.erase(stream);
  RTC_DCHECK_EQ(1, count);
  config_.audio_mixer->RemoveSource(stream->source());
  if (receiving_streams_.empty()) {
    config_.audio_device_module->StopPlayout();
  }
  UpdateNullAudioPollerState();
}

void AudioState::SetRecording(bool enabled) {
  RTC_LOG(LS_INFO) << "SetRecording(" << enabled << ")";
  RTC_DCHECK_RUN_ON(&thread_checker_);
  recording_enabled_ = enabled;
  UpdateRecordingState(!enabled);
}

void AudioState::AddSendingStream(webrtc::AudioSendStream* stream,
                                  int sample_rate_hz,
                                  size_t num_channels,
                                  bool muted) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  auto& properties = sending_streams_[stream];
  properties.sample_rate_hz = sample_rate_hz;
  properties.num_channels = num_channels;
  properties.muted = muted;
  UpdateAudioTransportWithSendingStreams();
  UpdateRecordingState();
}

void AudioState::RemoveSendingStream(webrtc::AudioSendStream* stream) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  auto count = sending_streams_.erase(stream);
  RTC_DCHECK_EQ(1, count);
  UpdateAudioTransportWithSendingStreams();
  UpdateRecordingState(sending_streams_.empty());
}

void AudioState::SetSendingStreamMuted(webrtc::AudioSendStream* stream,
                                       bool muted) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  auto it = sending_streams_.find(stream);
  RTC_DCHECK(it != sending_streams_.end());
  if (it == sending_streams_.end()) {
    return;
  }
  it->second.muted = muted;
  UpdateRecordingState();
}

void AudioState::UpdateRecordingState(bool force_stop) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  auto* adm = config_.audio_device_module.get();
  const bool has_native_senders = !sending_streams_.empty();
  const bool all_muted =
      std::all_of(sending_streams_.begin(), sending_streams_.end(),
                  [](const auto& kv) { return kv.second.muted; });
  if (config_.audio_processing) {
    config_.audio_processing->set_output_will_be_muted(
        has_native_senders && (!recording_enabled_ || all_muted));
  }

  if (!recording_enabled_ || !has_native_senders ||
      (all_muted && adm->IsStopOnMuteModeEnabled())) {
    // Explicit disable and final removal always release the device, including
    // a device whose failed initialization left partial resources behind.
    if (force_stop || adm->Recording() || adm->RecordingIsInitialized()) {
      adm->StopRecording();
    }
    return;
  }

  if (!adm->IsStopOnMuteModeEnabled()) {
    adm->SetMicrophoneMute(all_muted);
  }
  if (adm->Recording()) {
    return;
  }
  if (adm->InitRecording() != 0) {
    RTC_DLOG_F(LS_ERROR) << "Failed to initialize recording.";
    return;
  }
#if defined(WEBRTC_WIN)
  if (adm->BuiltInAECIsAvailable() && !adm->Playing()) {
    if (!adm->PlayoutIsInitialized()) {
      adm->InitPlayout();
    }
    adm->StartPlayout();
  }
#endif
  adm->StartRecording();
}

void AudioState::SetStereoChannelSwapping(bool enable) {
  RTC_DCHECK(thread_checker_.IsCurrent());
  audio_transport_.SetStereoChannelSwapping(enable);
}

void AudioState::UpdateAudioTransportWithSendingStreams() {
  RTC_DCHECK(thread_checker_.IsCurrent());
  std::vector<AudioSender*> audio_senders;
  int max_sample_rate_hz = 8000;
  size_t max_num_channels = 1;
  for (const auto& kv : sending_streams_) {
    audio_senders.push_back(kv.first);
    max_sample_rate_hz = std::max(max_sample_rate_hz, kv.second.sample_rate_hz);
    max_num_channels = std::max(max_num_channels, kv.second.num_channels);
  }
  audio_transport_.UpdateAudioSenders(std::move(audio_senders),
                                      max_sample_rate_hz, max_num_channels);
}

void AudioState::UpdateNullAudioPollerState() {
  // Run NullAudioPoller when there are receiving streams and playout is
  // disabled.
  if (!receiving_streams_.empty() && !playout_enabled_) {
    if (!null_audio_poller_.Running()) {
      AudioTransport* audio_transport = &audio_transport_;
      null_audio_poller_ = RepeatingTaskHandle::Start(
          TaskQueueBase::Current(), [audio_transport] {
            static constexpr size_t kNumChannels = 1;
            static constexpr uint32_t kSamplesPerSecond = 48'000;
            // 10ms of samples
            static constexpr size_t kNumSamples = kSamplesPerSecond / 100;

            // Buffer to hold the audio samples.
            int16_t buffer[kNumSamples * kNumChannels];

            // Output variables from `NeedMorePlayData`.
            size_t n_samples;
            int64_t elapsed_time_ms;
            int64_t ntp_time_ms;
            audio_transport->NeedMorePlayData(
                kNumSamples, sizeof(int16_t), kNumChannels, kSamplesPerSecond,
                buffer, n_samples, &elapsed_time_ms, &ntp_time_ms);

            // Reschedule the next poll iteration.
            return TimeDelta::Millis(10);
          });
    }
  } else {
    null_audio_poller_.Stop();
  }
}
}  // namespace internal

scoped_refptr<AudioState> AudioState::Create(const AudioState::Config& config) {
  return make_ref_counted<internal::AudioState>(config);
}
}  // namespace webrtc
