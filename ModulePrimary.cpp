/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>

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
using aidl::android::media::audio::common::AudioInputFlags;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

namespace {

// Accepted vendor parameter ids (module-level).
// Values are floats in range [-1.0, +1.0].
const std::string kCarBalanceId = "car.balance";
const std::string kCarFaderId = "car.fader";
// Legacy aliases (keep for compatibility with earlier experiments).
const std::string kCarBalanceIdLegacy = "car_balance";
const std::string kCarFaderIdLegacy = "car_fader";

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

ndk::ScopedAStatus ModulePrimary::getVendorParameters(const std::vector<std::string>& in_ids,
                                                      std::vector<VendorParameter>* _aidl_return) {
    LOG(VERBOSE) << __func__ << ": " << mType << ": id count: " << in_ids.size();
    bool allParametersKnown = true;
    for (const auto& id : in_ids) {
        if (id == kCarBalanceId || id == kCarBalanceIdLegacy) {
            VendorParameter p{.id = id};
            p.ext.setParcelable(Float{primary::PrimaryMixer::getBalance()});
            _aidl_return->push_back(std::move(p));
        } else if (id == kCarFaderId || id == kCarFaderIdLegacy) {
            VendorParameter p{.id = id};
            p.ext.setParcelable(Float{primary::PrimaryMixer::getFader()});
            _aidl_return->push_back(std::move(p));
        } else if (id == VendorDebug::kForceTransientBurstName) {
            VendorParameter p{.id = id};
            p.ext.setParcelable(Boolean{mVendorDebug.forceTransientBurst});
            _aidl_return->push_back(std::move(p));
        } else if (id == VendorDebug::kForceSynchronousDrainName) {
            VendorParameter p{.id = id};
            p.ext.setParcelable(Boolean{mVendorDebug.forceSynchronousDrain});
            _aidl_return->push_back(std::move(p));
        } else if (id == kClipTransitionSupportName) {
            VendorParameter p{.id = id};
            p.ext.setParcelable(Boolean{true});
            _aidl_return->push_back(std::move(p));
        } else {
            allParametersKnown = false;
            LOG(VERBOSE) << __func__ << ": " << mType << ": unrecognized parameter \"" << id
                         << "\"";
        }
    }
    if (allParametersKnown) return ndk::ScopedAStatus::ok();
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus ModulePrimary::setVendorParameters(
        const std::vector<VendorParameter>& in_parameters, bool in_async) {
    LOG(VERBOSE) << __func__ << ": " << mType << ": parameter count " << in_parameters.size()
                 << ", async: " << in_async;
    bool allParametersKnown = true;
    for (const auto& p : in_parameters) {
        if (p.id == kCarBalanceId || p.id == kCarBalanceIdLegacy) {
            float v = 0.0f;
            if (!extractParameter<Float>(p, &v)) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
            primary::PrimaryMixer::setBalance(v);
        } else if (p.id == kCarFaderId || p.id == kCarFaderIdLegacy) {
            float v = 0.0f;
            if (!extractParameter<Float>(p, &v)) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
            primary::PrimaryMixer::setFader(v);
        } else if (p.id == VendorDebug::kForceTransientBurstName) {
            if (!extractParameter<Boolean>(p, &mVendorDebug.forceTransientBurst)) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
        } else if (p.id == VendorDebug::kForceSynchronousDrainName) {
            if (!extractParameter<Boolean>(p, &mVendorDebug.forceSynchronousDrain)) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
        } else {
            allParametersKnown = false;
            LOG(VERBOSE) << __func__ << ": " << mType << ": unrecognized parameter \"" << p.id
                         << "\"";
        }
    }
    if (allParametersKnown) return ndk::ScopedAStatus::ok();
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
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
        // "Stub" is used because there is no support for MMAP audio I/O on CVD.
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
        // "Stub" is used because there is no support for MMAP audio I/O on CVD.
        return createStreamInstance<StreamOutMmapStub>(result, std::move(context), sourceMetadata,
                                                       offloadInfo);
    } else if (areAllBitPositionFlagsSet(
                       context.getFlags().get<AudioIoFlags::output>(),
                       {AudioOutputFlags::COMPRESS_OFFLOAD, AudioOutputFlags::NON_BLOCKING})) {
        // "Stub" is used because there is no actual decoder. The stream just
        // extracts the clip duration from the media file header and simulates
        // playback over time.
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
    // The actual mmap buffer for I/O is created after the stream exits standby, via
    // 'IStreamCommon.createMmapBuffer'. But we must return a valid file descriptor here because
    // 'MmapBufferDescriptor' can not contain a "null" fd.
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
    // 85 ms is chosen considering 4096 frames @ 48 kHz. This is the value which allows
    // the virtual Android device implementation to pass CTS. Hardware implementations
    // should have significantly lower latency.
    static constexpr int32_t kStandardLatencyMs = 85;
    return hasMmapFlag(portConfig.flags.value()) ? kLowLatencyMs : kStandardLatencyMs;
}

}  // namespace aidl::android::hardware::audio::core
