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
    // Center is typically treated as "front" content.
    g.fc = front;
    // LFE typically left untouched by balance/fader.
    g.lfe = 1.0f;
    // Side channels (7.1) are treated as "rear" in a car cabin.
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
void applyCarBalanceFaderInterleaved(Sample* buf, size_t frames, int channels,
                                     const CarGains& g) {
    // Android channel orders:
    // 2ch:  FL FR
    // 4.0:  FL FR BL BR
    // 5.1:  FL FR FC LFE BL BR
    // 7.1:  FL FR FC LFE BL BR SL SR
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
            // Unsupported format: do nothing.
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

// In core-impl/StreamPrimary.cpp

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
    if (isStub) {
        LOG(INFO) << __func__ << ": USING STUB!";
        return mStubDriver.start();
    }

    // --- FIX: ALIGN WITH BUILD.PROP (2048/4) ---
    // Android sends 4096 frames. We need a buffer larger than 4096 to avoid drops.
    // 2048 * 4 = 8192 frames. This is safe.
    
    auto& mutableConfig = const_cast<std::optional<struct pcm_config>&>(mConfig);

    if (!mutableConfig.has_value()) {
        mutableConfig = pcm_config{};
    }

    // 1. Force Stereo (Matches your current XML)
    mutableConfig->channels = 2; 

    // 2. Standard Sample Rate
    mutableConfig->rate = 48000;
    mutableConfig->format = PCM_FORMAT_S16_LE;

    // 3. BUFFER FIX: Matches your PRODUCT_PROPERTY_OVERRIDES
    mutableConfig->period_size = 2048; 
    mutableConfig->period_count = 4; 
    
    // 4. Thresholds (Standard TinyALSA logic)
    mutableConfig->start_threshold = 2048 * 2; // Start after 2 periods (safe)
    mutableConfig->stop_threshold = 2048 * 4;  // Stop when full
    mutableConfig->silence_threshold = 0;

    LOG(INFO) << __func__ << ": Forcing PCM Config: 2ch, 48kHz, Period=2048, Count=4 (Total 8192)";

    RETURN_STATUS_IF_ERROR(StreamAlsa::start());
    mStartTimeNs = ::android::uptimeNanos();
    mFramesSinceStart = 0;
    mSkipNextTransfer = false;
    return ::android::OK;
}

// Fixed transfer function: Eliminates Emulator sleep logic and forces ALSA write
::android::status_t StreamPrimary::transfer(void* buffer, size_t frameCount,
                                            size_t* actualFrameCount, int32_t* latencyMs) {
    if (isStubStreamOnWorker()) {
        return mStubDriver.transfer(buffer, frameCount, actualFrameCount, latencyMs);
    }

    // Apply Automotive Fader/Balance (if enabled)
    if (!mIsInput && mConfig.has_value()) {
        applyCarBalanceFader(buffer, frameCount, mConfig.value());
    }

    // Directly call StreamAlsa::transfer
    // This calls pcm_write, which BLOCKS until the hardware consumes the data.
    // This is correct behavior for real hardware (USB DAC).
    return StreamAlsa::transfer(buffer, frameCount, actualFrameCount, latencyMs);
}

::android::status_t StreamPrimary::refinePosition(StreamDescriptor::Position*) {
    // Since not all data is actually sent to the HAL, use the position maintained by Stream class
    // which accounts for all frames passed from / to the client.
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
    return {alsa::DeviceProfile{.card = primary::PrimaryMixer::getAlsaCard(),
                                .device = primary::PrimaryMixer::getAlsaDevice(),
                                .direction = mIsInput ? PCM_IN : PCM_OUT,
                                .isExternal = false}};
}

bool StreamPrimary::isStubStream() {
    std::lock_guard l(mLock);
    return mAlsaDeviceId == kStubDeviceId;
}

// Map logical AAOS BUS outputs to the primary ALSA device.
StreamPrimary::AlsaDeviceId StreamPrimary::getCardAndDeviceId(
        const std::vector<AudioDevice>& devices) {
    
    // --- FIX: Force Card 0, Device 0 for Primary ---
    // Using kDefaultCardAndDeviceId is risky if it defaults to -1.
    // We hardcode this to {0, 0} because we know your USB DAC is at Card 0.
    const AlsaDeviceId primaryOut{0, 0}; 

    if (devices.empty()) {
        return primaryOut;
    }
    const AudioDevice& dev = devices[0];
    
    // If the device is a BUS output (type or connection), map to primary
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
    
    // Parse explicit "CARD_x_DEV_y" overrides (keep existing logic)
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
    // Fallback to primary
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
        return kSimulateInput || device.type.type == AudioDeviceType::IN_TELEPHONY_RX ||
               device.type.type == AudioDeviceType::IN_FM_TUNER;
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
    // Due to rounding errors, round trip conversions between percents and indexed values may not
    // match.
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
    // Due to rounding errors, round trip conversions between percents and indexed values may not
    // match.
    if (volumes != in_channelVolumes) {
        LOG(WARNING) << __func__ << ": unmatched volumes: set: "
                     << ::android::internal::ToString(in_channelVolumes)
                     << ", from mixer: " << ::android::internal::ToString(volumes);
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core