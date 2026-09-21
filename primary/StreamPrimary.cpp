/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AHAL_StreamPrimary"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <cstdio>
#include <unistd.h>
#include <cmath>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <audio_utils/clock.h>
#include <error/Result.h>
#include <error/expected_utils.h>

#include "core-impl/StreamPrimary.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceDescription;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::MicrophoneInfo;
using android::base::GetBoolProperty;

namespace aidl::android::hardware::audio::core {

namespace {

struct CarGains {
    float fl = 1.0f;
    float fr = 1.0f;
    float fc = 1.0f;
    float lfe = 1.0f;
    float rl = 1.0f;
    float rr = 1.0f;
    float sl = 1.0f;
    float sr = 1.0f;
};

inline CarGains computeCarGains(float balance, float fader) {
    balance = std::clamp(balance, -1.0f, 1.0f);
    fader = std::clamp(fader, -1.0f, 1.0f);

    const float left = (balance <= 0.0f) ? 1.0f : (1.0f - balance);
    const float right = (balance >= 0.0f) ? 1.0f : (1.0f + balance);

    const float front = (fader >= 0.0f) ? 1.0f : (1.0f + fader);
    const float rear = (fader <= 0.0f) ? 1.0f : (1.0f - fader);

    CarGains g;
    g.fl = left * front;
    g.fr = right * front;
    g.rl = left * rear;
    g.rr = right * rear;
    g.fc = front;
    g.lfe = 1.0f;
    g.sl = left * rear;
    g.sr = right * rear;
    return g;
}

template <typename T>
inline T clampCast(float v) {
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(v);
    } else {
        const float lo = static_cast<float>(std::numeric_limits<T>::min());
        const float hi = static_cast<float>(std::numeric_limits<T>::max());
        return static_cast<T>(std::clamp(v, lo, hi));
    }
}

template <typename Sample>
void applySmartMirroringInterleaved(Sample* buf, size_t frames, int channels) {
    // 5.1 Channel Mapping (Standard Android/ALSA):
    // Index 0: FL
    // Index 1: FR
    // Index 2: Center
    // Index 3: LFE
    // Index 4: RL  <-- We want to fill this
    // Index 5: RR  <-- And this
    
    if (channels != 6) return;

    for (size_t i = 0; i < frames; ++i) {
        Sample* f = buf + i * channels;
        
        // Check if Rear channels are silent (or near silent)
        // This ensures we don't overwrite real 5.1 content (like a movie)
        // but we DO upmix YouTube/Spotify stereo music.
        if (f[4] == 0 && f[5] == 0) {
            f[4] = f[0]; // Copy Front Left to Rear Left
            f[5] = f[1]; // Copy Front Right to Rear Right
        }
    }
}

void applySmartMirroring(void* buffer, size_t frames, const pcm_config& cfg) {
    if (cfg.channels != 6) return;

    switch (cfg.format) {
        case PCM_FORMAT_S16_LE:
            applySmartMirroringInterleaved<int16_t>(
                static_cast<int16_t*>(buffer), frames, cfg.channels);
            break;
        case PCM_FORMAT_S32_LE:
            applySmartMirroringInterleaved<int32_t>(
                static_cast<int32_t*>(buffer), frames, cfg.channels);
            break;
        case PCM_FORMAT_FLOAT_LE:
            applySmartMirroringInterleaved<float>(
                static_cast<float*>(buffer), frames, cfg.channels);
            break;
        default:
            break;
    }
}

template <typename Sample>
void applyCarBalanceFaderInterleaved(Sample* buf, size_t frames, int channels,
                                     const CarGains& g) {
    for (size_t i = 0; i < frames; ++i) {
        Sample* f = buf + i * channels;
        if (channels >= 2) {
            f[0] = clampCast<Sample>(static_cast<float>(f[0]) * g.fl);
            f[1] = clampCast<Sample>(static_cast<float>(f[1]) * g.fr);
        }
        if (channels == 4) {
            f[2] = clampCast<Sample>(static_cast<float>(f[2]) * g.rl);
            f[3] = clampCast<Sample>(static_cast<float>(f[3]) * g.rr);
        } else if (channels == 6) {
            f[2] = clampCast<Sample>(static_cast<float>(f[2]) * g.fc);
            f[3] = clampCast<Sample>(static_cast<float>(f[3]) * g.lfe);
            f[4] = clampCast<Sample>(static_cast<float>(f[4]) * g.rl);
            f[5] = clampCast<Sample>(static_cast<float>(f[5]) * g.rr);
        } else if (channels == 8) {
            f[2] = clampCast<Sample>(static_cast<float>(f[2]) * g.fc);
            f[3] = clampCast<Sample>(static_cast<float>(f[3]) * g.lfe);
            f[4] = clampCast<Sample>(static_cast<float>(f[4]) * g.rl);
            f[5] = clampCast<Sample>(static_cast<float>(f[5]) * g.rr);
            f[6] = clampCast<Sample>(static_cast<float>(f[6]) * g.sl);
            f[7] = clampCast<Sample>(static_cast<float>(f[7]) * g.sr);
        }
    }
}

template <typename Sample>
void applyGainInterleaved(Sample* buf, size_t frames, int channels, float gainLinear) {
    for (size_t i = 0; i < frames * channels; ++i) {
        buf[i] = clampCast<Sample>(static_cast<float>(buf[i]) * gainLinear);
    }
}

void applyCarBalanceFader(void* buffer, size_t frames, const pcm_config& cfg) {
    const int ch = cfg.channels;
    if (!(ch == 2 || ch == 4 || ch == 6 || ch == 8)) return;

    const float balance = primary::PrimaryMixer::getBalance();
    const float fader = primary::PrimaryMixer::getFader();
    if (balance == 0.0f && fader == 0.0f) return;

    const CarGains g = computeCarGains(balance, fader);
    switch (cfg.format) {
        case PCM_FORMAT_S16_LE:
            applyCarBalanceFaderInterleaved<int16_t>(static_cast<int16_t*>(buffer), frames, ch, g);
            break;
        case PCM_FORMAT_S32_LE:
            applyCarBalanceFaderInterleaved<int32_t>(static_cast<int32_t*>(buffer), frames, ch, g);
            break;
        case PCM_FORMAT_FLOAT_LE:
            applyCarBalanceFaderInterleaved<float>(static_cast<float*>(buffer), frames, ch, g);
            break;
        default:
            break;
    }
}

void applyBusVolume(void* buffer, size_t frames, const pcm_config& cfg, float gainDb) {
    // 0dB means no change. -inf means silence.
    if (gainDb > -0.01f && gainDb < 0.01f) return;

    // Convert dB to linear scalar: 10 ^ (db / 20)
    // If gainDb is very low (e.g. -100), treat as mute
    float gainLinear = 0.0f;
    if (gainDb > -90.0f) {
        gainLinear = std::pow(10.0f, gainDb / 20.0f);
    }

    const int ch = cfg.channels;
    switch (cfg.format) {
        case PCM_FORMAT_S16_LE:
            applyGainInterleaved<int16_t>(static_cast<int16_t*>(buffer), frames, ch, gainLinear);
            break;
        case PCM_FORMAT_S32_LE:
            applyGainInterleaved<int32_t>(static_cast<int32_t*>(buffer), frames, ch, gainLinear);
            break;
        case PCM_FORMAT_FLOAT_LE:
            applyGainInterleaved<float>(static_cast<float*>(buffer), frames, ch, gainLinear);
            break;
        default:
            break;
    }
}

}  // namespace

StreamPrimary::StreamPrimary(StreamContext* context, const Metadata& metadata)
    : StreamAlsa(context, metadata, 3 /*readWriteRetries*/),
      mIsAsynchronous(!!getContext().getAsyncCallback()),
      mStubDriver(getContext()) {
    context->startStreamDataProcessor();
}

::android::status_t StreamPrimary::init(DriverCallbackInterface* callback) {
    RETURN_STATUS_IF_ERROR(mStubDriver.init(callback));
    return StreamAlsa::init(callback);
}

::android::status_t StreamPrimary::drain(StreamDescriptor::DrainMode mode) {
    return isStubStreamOnWorker() ? mStubDriver.drain(mode) : StreamAlsa::drain(mode);
}

::android::status_t StreamPrimary::flush() {
    RETURN_STATUS_IF_ERROR(isStubStreamOnWorker() ? mStubDriver.flush() : StreamAlsa::flush());
    // TODO(b/372951987): consider if this needs to be done from 'StreamInWorkerLogic::cycle'.
    return mIsInput ? standby() : ::android::OK;
}

::android::status_t StreamPrimary::pause() {
    return isStubStreamOnWorker() ? mStubDriver.pause() : StreamAlsa::pause();
}

::android::status_t StreamPrimary::standby() {
    return isStubStreamOnWorker() ? mStubDriver.standby() : StreamAlsa::standby();
}

::android::status_t StreamPrimary::start() {
    bool isStub = true, shutdownAlsaStream = false;
    {
        std::lock_guard l(mLock);
        isStub = mAlsaDeviceId == kStubDeviceId;
        shutdownAlsaStream =
                mCurrAlsaDeviceId != mAlsaDeviceId && mCurrAlsaDeviceId != kStubDeviceId;
        mCurrAlsaDeviceId = mAlsaDeviceId;
    }
    if (shutdownAlsaStream) {
        StreamAlsa::shutdown();
    }
    if (!isStub) {
        // The sound card can be absent (e.g. the USB audio interface isn't plugged in). Run this
        // start on the stub driver instead of failing the stream; the next start re-checks, so a
        // card plugged in later is picked up.
        const auto profiles = getDeviceProfiles();
        for (const auto& profile : profiles) {
            char path[32];
            snprintf(path, sizeof(path), "/proc/asound/card%d", profile.card);
            if (profile.card < 0 || access(path, F_OK) != 0) {
                LOG(WARNING) << __func__ << ": ALSA card " << profile.card
                             << " not present, using the stub driver";
                isStub = true;
                if (mCurrAlsaDeviceId != kStubDeviceId) {
                    StreamAlsa::shutdown();  // Drop proxies of a card that has gone away.
                }
                mCurrAlsaDeviceId = kStubDeviceId;
                break;
            }
        }
    }
    if (isStub) {
        return mStubDriver.start();
    }

    // --- SMART CONFIGURATION FIX ---
    int actualChannels = 2;
    auto layout = mContext.getChannelLayout();
    using Tag = aidl::android::media::audio::common::AudioChannelLayout::Tag;
    switch (layout.getTag()) {
        case Tag::layoutMask:
            actualChannels = __builtin_popcount(layout.get<Tag::layoutMask>());
            break;
        case Tag::indexMask:
            actualChannels = __builtin_popcount(layout.get<Tag::indexMask>());
            break;
        case Tag::voiceMask:
            actualChannels = __builtin_popcount(layout.get<Tag::voiceMask>());
            break;
        default:
            LOG(WARNING) << __func__ << ": Unknown layout tag, using default 2ch";
            break;
    }

    int periodSize = ::android::base::GetIntProperty("ro.vendor.audio.period_size", 1024);
    int periodCount = ::android::base::GetIntProperty("ro.vendor.audio.period_count", 4);

    auto& mutableConfig = const_cast<std::optional<struct pcm_config>&>(mConfig);
    if (!mutableConfig.has_value()) {
        mutableConfig = pcm_config{};
    }

    mutableConfig->channels = actualChannels; 
    mutableConfig->rate = 48000;
    mutableConfig->format = PCM_FORMAT_S16_LE;
    mutableConfig->period_size = periodSize;
    mutableConfig->period_count = periodCount;
    mutableConfig->start_threshold = periodSize * 2; 
    mutableConfig->stop_threshold = periodSize * periodCount;
    mutableConfig->silence_threshold = 0;

    LOG(INFO) << __func__ << ": Configured PCM: " << actualChannels << "ch, 48kHz, Period=" 
              << periodSize << ", Count=" << periodCount;

    RETURN_STATUS_IF_ERROR(StreamAlsa::start());
    mStartTimeNs = ::android::uptimeNanos();
    mFramesSinceStart = 0;
    mSkipNextTransfer = false;
    return ::android::OK;
}

::android::status_t StreamPrimary::transfer(void* buffer, size_t frameCount,
                                            size_t* actualFrameCount, int32_t* latencyMs) {
    if (isStubStreamOnWorker()) {
        return mStubDriver.transfer(buffer, frameCount, actualFrameCount, latencyMs);
    }
    if (!mSkipNextTransfer) {
        if (!mIsInput && mConfig.has_value()) {
            // 1. Apply Volume Gain (Software Mixer) if bus address is known
            if (!mBusAddress.empty()) {
                float gainDb = primary::PrimaryMixer::getBusGain(mBusAddress);
                applyBusVolume(buffer, frameCount, mConfig.value(), gainDb);
            }

            // [NEW] 1.5 Apply Smart Mirroring (Stereo -> 5.1 Upmix)
            // This detects if rears are silent and fills them with front audio
            applySmartMirroring(buffer, frameCount, mConfig.value());

            // 2. Apply Balance/Fader
            applyCarBalanceFader(buffer, frameCount, mConfig.value());
        }
        RETURN_STATUS_IF_ERROR(
                StreamAlsa::transfer(buffer, frameCount, actualFrameCount, latencyMs));
    } else {
        LOG(DEBUG) << __func__ << ": skipping transfer (" << frameCount << " frames)";
        *actualFrameCount = frameCount;
        if (mIsInput) memset(buffer, 0, frameCount * mFrameSizeBytes);
        mSkipNextTransfer = false;
    }
    if (!mIsAsynchronous) {
        const long bufferDurationUs =
                (*actualFrameCount) * MICROS_PER_SECOND / mContext.getSampleRate();
        const auto totalDurationUs =
                (::android::uptimeNanos() - mStartTimeNs) / NANOS_PER_MICROSECOND;
        mFramesSinceStart += *actualFrameCount;
        const long totalOffsetUs =
                mFramesSinceStart * MICROS_PER_SECOND / mContext.getSampleRate() - totalDurationUs;
        LOG(VERBOSE) << __func__ << ": totalOffsetUs " << totalOffsetUs;
        if (totalOffsetUs > 0) {
            const long sleepTimeUs = std::min(totalOffsetUs, bufferDurationUs);
            LOG(VERBOSE) << __func__ << ": sleeping for " << sleepTimeUs << " us";
            usleep(sleepTimeUs);
        } else {
            mSkipNextTransfer = true;
        }
    } else {
        LOG(VERBOSE) << __func__ << ": asynchronous transfer";
    }
    return ::android::OK;
}

::android::status_t StreamPrimary::refinePosition(StreamDescriptor::Position*) {
    return ::android::OK;
}

void StreamPrimary::shutdown() {
    StreamAlsa::shutdown();
    mStubDriver.shutdown();
}

ndk::ScopedAStatus StreamPrimary::setConnectedDevices(const ConnectedDevices& devices) {
    LOG(DEBUG) << __func__ << ": " << ::android::internal::ToString(devices);
    if (devices.size() > 1) {
        LOG(ERROR) << __func__ << ": primary stream can only be connected to one device, got: "
                   << devices.size();
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    {
        const bool useStubDriver = devices.empty() || useStubStream(mIsInput, devices[0]);
        std::lock_guard l(mLock);
        mAlsaDeviceId = useStubDriver ? kStubDeviceId : getCardAndDeviceId(devices);
        
        // --- Capture Bus Address for Volume Control ---
        mBusAddress.clear();
        mIsFmTunerInput = false;
        if (!devices.empty()) {
             const auto& dev = devices[0];
             // If the device is an output bus, capture its address (e.g. "bus0_media_out")
             if (!mIsInput && (dev.type.type == AudioDeviceType::OUT_BUS ||
                               dev.type.connection == AudioDeviceDescription::CONNECTION_BUS)) {
                 if (dev.address.getTag() == AudioDeviceAddress::id) {
                     mBusAddress = dev.address.get<AudioDeviceAddress::id>();
                     LOG(INFO) << __func__ << ": Stream bound to Bus Address: " << mBusAddress;
                 }
             }
             // Native radio: the FM tuner input is fed by the RTL-SDR HAL via snd-aloop,
             // so it must capture from the Loopback card (handled in getDeviceProfiles).
             if (mIsInput && dev.type.type == AudioDeviceType::IN_FM_TUNER) {
                 mIsFmTunerInput = true;
                 LOG(INFO) << __func__ << ": FM tuner input -> snd-aloop Loopback capture";
             }
        }
    }
    if (!devices.empty()) {
        auto streamDataProcessor = getContext().getStreamDataProcessor().lock();
        if (streamDataProcessor != nullptr) {
            streamDataProcessor->setAudioDevice(devices[0]);
        }
    }
    return StreamAlsa::setConnectedDevices(devices);
}

std::vector<alsa::DeviceProfile> StreamPrimary::getDeviceProfiles() {
    // Native radio: the FM tuner input captures the PCM the RTL-SDR HAL plays into the
    // snd-aloop "Loopback" card. snd-aloop mirrors device 0 playback <-> device 1 capture,
    // so the radio daemon writes hw:Loopback,0,0 and we read hw:Loopback,1,0. Resolve the
    // card by NAME so the pinned snd_aloop.index value never has to be hard-coded here.
    if (mIsInput && mIsFmTunerInput) {
        const int loopCard = primary::PrimaryMixer::findAlsaCardByName("Loopback");
        if (loopCard == primary::PrimaryMixer::kInvalidAlsaCard) {
            LOG(ERROR) << __func__ << ": Loopback card not found; FM tuner capture will fail";
        }
        return {alsa::DeviceProfile{.card = loopCard,
                                    .device = 1,
                                    .direction = PCM_IN,
                                    .isExternal = false}};
    }
    return {alsa::DeviceProfile{.card = primary::PrimaryMixer::getAlsaCard(),
                                .device = primary::PrimaryMixer::getAlsaDevice(),
                                .direction = mIsInput ? PCM_IN : PCM_OUT,
                                .isExternal = false}};
}

bool StreamPrimary::isStubStream() {
    std::lock_guard l(mLock);
    return mAlsaDeviceId == kStubDeviceId;
}

StreamPrimary::AlsaDeviceId StreamPrimary::getCardAndDeviceId(
        const std::vector<AudioDevice>& devices) {
    const AlsaDeviceId primaryOut{0, 0}; 

    if (devices.empty()) {
        return primaryOut;
    }
    const AudioDevice& dev = devices[0];
    
    if (dev.type.type == AudioDeviceType::OUT_BUS ||
        dev.type.connection == AudioDeviceDescription::CONNECTION_BUS) {
        
        // Log the mapping so we can debug it in logcat
        std::string addr = dev.address.getTag() == AudioDeviceAddress::id
                                         ? dev.address.get<AudioDeviceAddress::id>()
                                         : std::string("<none>");
        LOG(INFO) << __func__ << ": mapping OUT_BUS device '" << addr
                  << "' to physical Card 0, Device 0";
                  
        return primaryOut;
    }
    
    if (dev.address.getTag() == AudioDeviceAddress::id) {
        std::string deviceAddress = dev.address.get<AudioDeviceAddress::id>();
        AlsaDeviceId cardAndDeviceId;
        if (const size_t suffixPos = deviceAddress.rfind("CARD_");
            suffixPos != std::string::npos &&
            sscanf(deviceAddress.c_str() + suffixPos, "CARD_%d_DEV_%d",
                   &cardAndDeviceId.first, &cardAndDeviceId.second) == 2) {
            LOG(INFO) << __func__ << ": parsed address '" << deviceAddress
                      << "' as card=" << cardAndDeviceId.first << ", device="
                      << cardAndDeviceId.second;
            return cardAndDeviceId;
        }
    }
    return primaryOut;
}

// static
bool StreamPrimary::useStubStream(
        bool isInput, const ::aidl::android::media::audio::common::AudioDevice& device) {
    static const bool kSimulateInput =
            GetBoolProperty("ro.boot.audio.tinyalsa.simulate_input", false);
    static const bool kSimulateOutput =
            GetBoolProperty("ro.boot.audio.tinyalsa.ignore_output", false);
    if (isInput) {
        // IN_FM_TUNER is intentionally NOT stubbed: the native radio path captures real
        // PCM from the snd-aloop Loopback card (see getDeviceProfiles / mIsFmTunerInput).
        return kSimulateInput || device.type.type == AudioDeviceType::IN_TELEPHONY_RX;
    }
    return kSimulateOutput || device.type.type == AudioDeviceType::OUT_TELEPHONY_TX;
}

StreamInPrimary::StreamInPrimary(StreamContext&& context, const SinkMetadata& sinkMetadata,
                                 const std::vector<MicrophoneInfo>& microphones)
    : StreamIn(std::move(context), microphones),
      StreamPrimary(&mContextInstance, sinkMetadata),
      StreamInHwGainHelper(&mContextInstance) {}

ndk::ScopedAStatus StreamInPrimary::getHwGain(std::vector<float>* _aidl_return) {
    if (isStubStream()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (mHwGains.empty()) {
        float gain;
        RETURN_STATUS_IF_ERROR(primary::PrimaryMixer::getInstance().getMicGain(&gain));
        _aidl_return->resize(mChannelCount, gain);
        RETURN_STATUS_IF_ERROR(setHwGainImpl(*_aidl_return));
    }
    return getHwGainImpl(_aidl_return);
}

ndk::ScopedAStatus StreamInPrimary::setHwGain(const std::vector<float>& in_channelGains) {
    if (isStubStream()) {
        LOG(DEBUG) << __func__ << ": gains " << ::android::internal::ToString(in_channelGains);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    auto currentGains = mHwGains;
    RETURN_STATUS_IF_ERROR(setHwGainImpl(in_channelGains));
    if (in_channelGains.size() < 1) {
        LOG(FATAL) << __func__ << ": unexpected gain vector size: " << in_channelGains.size();
    }
    if (auto status = primary::PrimaryMixer::getInstance().setMicGain(in_channelGains[0]);
        !status.isOk()) {
        mHwGains = currentGains;
        return status;
    }
    float gain;
    RETURN_STATUS_IF_ERROR(primary::PrimaryMixer::getInstance().getMicGain(&gain));
    if (gain != in_channelGains[0]) {
        LOG(WARNING) << __func__ << ": unmatched gain: set: " << in_channelGains[0]
                     << ", from mixer: " << gain;
    }
    return ndk::ScopedAStatus::ok();
}

StreamOutPrimary::StreamOutPrimary(StreamContext&& context, const SourceMetadata& sourceMetadata,
                                   const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamPrimary(&mContextInstance, sourceMetadata),
      StreamOutHwVolumeHelper(&mContextInstance) {}

ndk::ScopedAStatus StreamOutPrimary::getHwVolume(std::vector<float>* _aidl_return) {
    if (isStubStream()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (mHwVolumes.empty()) {
        RETURN_STATUS_IF_ERROR(primary::PrimaryMixer::getInstance().getVolumes(_aidl_return));
        _aidl_return->resize(mChannelCount);
        RETURN_STATUS_IF_ERROR(setHwVolumeImpl(*_aidl_return));
    }
    return getHwVolumeImpl(_aidl_return);
}

ndk::ScopedAStatus StreamOutPrimary::setHwVolume(const std::vector<float>& in_channelVolumes) {
    if (isStubStream()) {
        LOG(DEBUG) << __func__ << ": volumes " << ::android::internal::ToString(in_channelVolumes);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    auto currentVolumes = mHwVolumes;
    RETURN_STATUS_IF_ERROR(setHwVolumeImpl(in_channelVolumes));
    if (auto status = primary::PrimaryMixer::getInstance().setVolumes(in_channelVolumes);
        !status.isOk()) {
        mHwVolumes = currentVolumes;
        return status;
    }
    std::vector<float> volumes;
    RETURN_STATUS_IF_ERROR(primary::PrimaryMixer::getInstance().getVolumes(&volumes));
    if (volumes != in_channelVolumes) {
        LOG(WARNING) << __func__ << ": unmatched volumes: set: "
                     << ::android::internal::ToString(in_channelVolumes)
                     << ", from mixer: " << ::android::internal::ToString(volumes);
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core
