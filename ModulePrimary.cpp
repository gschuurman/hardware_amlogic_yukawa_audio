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

#include <vector>
#include <cmath>

#include <android/binder_parcelable_utils.h>

#define LOG_TAG "AHAL_ModulePrimary"
#include <Utils.h>
#include <android-base/logging.h>

#include "core-impl/ModulePrimary.h"
#include "primary/PrimaryMixer.h"
#include "core-impl/StreamMmapStub.h"
#include "core-impl/StreamOffloadStub.h"
#include "core-impl/StreamPrimary.h"
#include "core-impl/Telephony.h"

using aidl::android::media::audio::common::Boolean;
using aidl::android::media::audio::common::Float;

using aidl::android::hardware::audio::common::areAllBitPositionFlagsSet;
using aidl::android::hardware::audio::common::hasMmapFlag;
using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::hardware::audio::core::StreamDescriptor;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioInputFlags;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::MicrophoneInfo;
using aidl::android::hardware::audio::core::VendorParameter;
using aidl::android::media::audio::common::Float;

namespace aidl::android::hardware::audio::core {

namespace {

// Accepted vendor parameter ids (module-level).
// Values are floats in range [-1.0, +1.0].
const std::string kCarBalanceId = "car.balance";
const std::string kCarFaderId = "car.fader";

// Equalizer: "car.eq.enabled" (0/1), "car.eq.preamp" and "car.eq.band<N>" (gains in dB).
const std::string kCarEqEnabledId = "car.eq.enabled";
const std::string kCarEqPreampId = "car.eq.preamp";
const std::string kCarEqBandPrefix = "car.eq.band";

// Returns the band index of a "car.eq.band<N>" id, or -1.
inline int carEqBand(const std::string& id) {
    if (id.rfind(kCarEqBandPrefix, 0) != 0 || id.size() != kCarEqBandPrefix.size() + 1) return -1;
    const int band = id.back() - '0';
    return (band >= 0 && band < primary::PrimaryMixer::kEqBands) ? band : -1;
}

inline bool isCarParam(const std::string& id) {
    return id == kCarBalanceId || id == kCarFaderId || id == kCarEqEnabledId ||
           id == kCarEqPreampId || carEqBand(id) >= 0;
}

// Helper to detect bus volume parameters (e.g. "bus0_media_out")
inline bool isBusParam(const std::string& id) {
    return id.find("bus") == 0;
}

inline bool readFloatParam(const VendorParameter& p, float* out) {
    std::optional<::aidl::android::media::audio::common::Float> f;
    const binder_status_t st = p.ext.getParcelable(&f);
    if (st != STATUS_OK || !f.has_value()) {
        LOG(ERROR) << __func__ << ": failed to read Float for \"" << p.id
                   << "\" binder_status=" << st << " has_value=" << (f.has_value() ? "true" : "false");
        return false;
    }
    *out = f->value;
    return true;
}

inline void writeFloatParam(VendorParameter* p, float v) {
    (void)p->ext.setParcelable(::aidl::android::media::audio::common::Float{v});
}

template <typename W>
bool extractParameter(const VendorParameter& p, decltype(W::value)* v) {
    std::optional<W> value;
    binder_status_t result = p.ext.getParcelable(&value);
    if (result == STATUS_OK && value.has_value()) {
        *v = value.value().value;
        return true;
    }
    LOG(ERROR) << __func__ << ": failed to read the value of the parameter \"" << p.id
               << "\": " << result;
    return false;
}

}  // namespace

ModulePrimary::ModulePrimary(std::unique_ptr<Configuration>&& config)
    : Module(Type::DEFAULT, std::move(config)) {
    // Force PrimaryMixer initialization to apply mixer controls at boot
    (void)primary::PrimaryMixer::getInstance();
}

ndk::ScopedAStatus ModulePrimary::getVendorParameters(
        const std::vector<std::string>& in_ids,
        std::vector<VendorParameter>* _aidl_return) {

    std::vector<std::string> passthrough;
    passthrough.reserve(in_ids.size());

    for (const auto& id : in_ids) {
        if (isCarParam(id)) {
            float value = 0.0f;
            if (id == kCarBalanceId) {
                value = primary::PrimaryMixer::getBalance();
            } else if (id == kCarFaderId) {
                value = primary::PrimaryMixer::getFader();
            } else if (id == kCarEqEnabledId) {
                value = primary::PrimaryMixer::getEqEnabled() ? 1.0f : 0.0f;
            } else if (id == kCarEqPreampId) {
                value = primary::PrimaryMixer::getEqPreamp();
            } else {
                value = primary::PrimaryMixer::getEqBandGain(carEqBand(id));
            }
            VendorParameter p;
            p.id = id;
            writeFloatParam(&p, value);
            _aidl_return->push_back(std::move(p));
        } else if (isBusParam(id)) {
            VendorParameter p;
            p.id = id;
            float val = primary::PrimaryMixer::getBusGain(id);
            writeFloatParam(&p, val);
            _aidl_return->push_back(std::move(p));
        } else {
            passthrough.push_back(id);
        }
    }

    // Delegate everything else to the base Module implementation.
    if (!passthrough.empty()) {
        std::vector<VendorParameter> baseOut;
        auto status = Module::getVendorParameters(passthrough, &baseOut);
        if (!status.isOk()) return status;
        _aidl_return->insert(_aidl_return->end(),
                             std::make_move_iterator(baseOut.begin()),
                             std::make_move_iterator(baseOut.end()));
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModulePrimary::setVendorParameters(
        const std::vector<VendorParameter>& in_parameters,
        bool in_async) {

    std::vector<VendorParameter> passthrough;
    passthrough.reserve(in_parameters.size());

    for (const auto& p : in_parameters) {
        if (isCarParam(p.id)) {
            float raw = 0.0f;
            if (!readFloatParam(p, &raw) || !std::isfinite(raw)) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
            if (p.id == kCarBalanceId) {
                primary::PrimaryMixer::setBalance(raw);
            } else if (p.id == kCarFaderId) {
                primary::PrimaryMixer::setFader(raw);
            } else if (p.id == kCarEqEnabledId) {
                primary::PrimaryMixer::setEqEnabled(raw != 0.0f);
            } else if (p.id == kCarEqPreampId) {
                primary::PrimaryMixer::setEqPreamp(raw);
            } else {
                primary::PrimaryMixer::setEqBandGain(carEqBand(p.id), raw);
            }
        } else if (isBusParam(p.id)) {
            float raw = 0.0f;
            if (readFloatParam(p, &raw)) {
                 primary::PrimaryMixer::setBusGain(p.id, raw);
            } else {
                LOG(ERROR) << "ModulePrimary: Failed to read float for bus param: " << p.id;
            }
        } else {
            passthrough.push_back(p);
        }
    }

    if (!passthrough.empty()) {
        auto status = Module::setVendorParameters(passthrough, in_async);
        if (!status.isOk()) return status;
    }

    return ndk::ScopedAStatus::ok();
}

// [FIX] Intercept setAudioPortConfig to handle Quick Settings volume (Hardware Volume)
ndk::ScopedAStatus ModulePrimary::setAudioPortConfig(
        const AudioPortConfig& in_requested,
        AudioPortConfig* out_suggested,
        bool* _aidl_return) {
    
    // 1. Let the base module handle the logic (finding port, creating config, applying to streams)
    auto status = Module::setAudioPortConfig(in_requested, out_suggested, _aidl_return);
    
    // 2. If successful and a change occurred, check for Gain updates
    if (status.isOk() && _aidl_return && *(_aidl_return)) {
        // Use the output 'out_suggested' which contains the fully merged configuration
        if (out_suggested->gain.has_value() && 
            out_suggested->ext.getTag() == AudioPortExt::Tag::device) {
            
            const auto& gainConfig = out_suggested->gain.value();
            const auto& deviceExt = out_suggested->ext.get<AudioPortExt::Tag::device>();

            // Check if this is a bus device (address matches "bus...")
            if (deviceExt.device.address.getTag() == AudioDeviceAddress::id) {
                std::string busAddress = deviceExt.device.address.get<AudioDeviceAddress::id>();
                
                if (!busAddress.empty() && !gainConfig.values.empty()) {
                    // Framework sends gain in Millibels (mB)
                    int gainMb = gainConfig.values[0];
                    float gainDb = gainMb / 100.0f;

                    LOG(INFO) << "ModulePrimary: setAudioPortConfig captured volume for " 
                              << busAddress << ": " << gainDb << "dB (" << gainMb << "mB)";
                    
                    // Unified Control: Update the same mixer used by AudioControl
                    primary::PrimaryMixer::setBusGain(busAddress, gainDb);
                }
            }
        }
    }
    return status;
}

ndk::ScopedAStatus ModulePrimary::getTelephony(std::shared_ptr<ITelephony>* _aidl_return) {
    if (!mTelephony) {
        mTelephony = ndk::SharedRefBase::make<Telephony>();
    }
    *_aidl_return = mTelephony.getInstance();
    LOG(DEBUG) << __func__
               << ": returning instance of ITelephony: " << _aidl_return->get()->asBinder().get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModulePrimary::calculateBufferSizeFrames(
        const ::aidl::android::media::audio::common::AudioFormatDescription& format,
        int32_t latencyMs, int32_t sampleRateHz, int32_t* bufferSizeFrames) {
    if (format.type != ::aidl::android::media::audio::common::AudioFormatType::PCM &&
        StreamOffloadStub::getSupportedEncodings().count(format.encoding)) {
        *bufferSizeFrames = sampleRateHz / 2;  // 1/2 of a second.
        return ndk::ScopedAStatus::ok();
    }
    return Module::calculateBufferSizeFrames(format, latencyMs, sampleRateHz, bufferSizeFrames);
}

ndk::ScopedAStatus ModulePrimary::createInputStream(StreamContext&& context,
                                                    const SinkMetadata& sinkMetadata,
                                                    const std::vector<MicrophoneInfo>& microphones,
                                                    std::shared_ptr<StreamIn>* result) {
    if (context.isMmap()) {
        return createStreamInstance<StreamInMmapStub>(result, std::move(context), sinkMetadata,
                                                      microphones);
    }
    return createStreamInstance<StreamInPrimary>(result, std::move(context), sinkMetadata,
                                                 microphones);
}

ndk::ScopedAStatus ModulePrimary::createOutputStream(
        StreamContext&& context, const SourceMetadata& sourceMetadata,
        const std::optional<AudioOffloadInfo>& offloadInfo, std::shared_ptr<StreamOut>* result) {
    if (context.isMmap()) {
        return createStreamInstance<StreamOutMmapStub>(result, std::move(context), sourceMetadata,
                                                       offloadInfo);
    } else if (areAllBitPositionFlagsSet(
                       context.getFlags().get<AudioIoFlags::output>(),
                       {AudioOutputFlags::COMPRESS_OFFLOAD, AudioOutputFlags::NON_BLOCKING})) {
        return createStreamInstance<StreamOutOffloadStub>(result, std::move(context),
                                                          sourceMetadata, offloadInfo);
    }
    return createStreamInstance<StreamOutPrimary>(result, std::move(context), sourceMetadata,
                                                  offloadInfo);
}

ndk::ScopedAStatus ModulePrimary::createMmapBuffer(const AudioPortConfig& portConfig,
                                                   int32_t bufferSizeFrames, int32_t frameSizeBytes,
                                                   MmapBufferDescriptor* desc) {
    const size_t bufferSizeBytes = static_cast<size_t>(bufferSizeFrames) * frameSizeBytes;
    const std::string regionName =
            std::string("mmap-sim-o-") +
            std::to_string(portConfig.ext.get<AudioPortExt::Tag::mix>().handle);
    int fd = ashmem_create_region(regionName.c_str(), bufferSizeBytes);
    if (fd < 0) {
        PLOG(ERROR) << __func__ << ": failed to create shared memory region of " << bufferSizeBytes
                    << " bytes";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    desc->sharedMemory.fd = ndk::ScopedFileDescriptor(fd);
    desc->sharedMemory.size = bufferSizeBytes;
    desc->burstSizeFrames = bufferSizeFrames / 2;
    desc->flags = 0;
    LOG(DEBUG) << __func__ << ": " << desc->toString();
    return ndk::ScopedAStatus::ok();
}

int32_t ModulePrimary::getNominalLatencyMs(const AudioPortConfig& portConfig) {
    static constexpr int32_t kLowLatencyMs = 5;
    static constexpr int32_t kStandardLatencyMs = 85;
    return hasMmapFlag(portConfig.flags.value()) ? kLowLatencyMs : kStandardLatencyMs;
}

}  // namespace aidl::android::hardware::audio::core