/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "primary/CarEqualizer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <type_traits>

namespace aidl::android::hardware::audio::core::primary {

namespace {

// Bandwidth of each peaking band. With bands two octaves apart, Q = 0.9 keeps
// neighbouring bands overlapping without large bumps between them.
constexpr float kEqQ = 0.9f;

}  // namespace

bool CarEqualizer::update(unsigned int sampleRate, int channels) {
    const uint32_t generation = PrimaryMixer::getEqGeneration();
    if (generation == mGeneration && sampleRate == mSampleRate && channels == mChannels) {
        return mActive;
    }
    mGeneration = generation;
    mSampleRate = sampleRate;
    mChannels = channels;
    mBandCount = 0;
    mActive = false;
    mPreamp = 1.0f;

    if (!PrimaryMixer::getEqEnabled() || sampleRate == 0 || channels <= 0) return false;

    // Boosts are compensated by lowering the input, so a boosted band can't clip.
    float maxBoostDb = 0.0f;
    for (int band = 0; band < PrimaryMixer::kEqBands; ++band) {
        const float gainDb = PrimaryMixer::getEqBandGain(band);
        maxBoostDb = std::max(maxBoostDb, gainDb);
        const float frequency = PrimaryMixer::kEqFrequenciesHz[band];
        // Flat bands and bands at or above Nyquist are left out.
        if (gainDb == 0.0f || frequency >= 0.45f * sampleRate) continue;

        // Peaking EQ from the Audio EQ Cookbook (R. Bristow-Johnson).
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * frequency / sampleRate;
        const double alpha = std::sin(w0) / (2.0 * kEqQ);
        const double cosw0 = std::cos(w0);
        const double a0 = 1.0 + alpha / a;
        Biquad& f = mFilters[mBandCount++];
        f.b0 = static_cast<float>((1.0 + alpha * a) / a0);
        f.b1 = static_cast<float>(-2.0 * cosw0 / a0);
        f.b2 = static_cast<float>((1.0 - alpha * a) / a0);
        f.a1 = static_cast<float>(-2.0 * cosw0 / a0);
        f.a2 = static_cast<float>((1.0 - alpha / a) / a0);
    }
    if (mBandCount == 0) return false;

    mPreamp = std::pow(10.0f, -maxBoostDb / 20.0f);
    mState.assign(static_cast<size_t>(channels) * mBandCount * 2, 0.0f);
    mActive = true;
    return true;
}

float CarEqualizer::responseDb(float frequencyHz) const {
    if (!mActive) return 0.0f;
    const double w = 2.0 * M_PI * frequencyHz / mSampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    std::complex<double> h = mPreamp;
    for (size_t i = 0; i < mBandCount; ++i) {
        const Biquad& f = mFilters[i];
        const double b0 = f.b0, b1 = f.b1, b2 = f.b2, a1 = f.a1, a2 = f.a2;
        h *= (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }
    return static_cast<float>(20.0 * std::log10(std::abs(h)));
}

template <typename Sample>
void CarEqualizer::processInterleaved(Sample* buffer, size_t frames, float toFloat,
                                      float fromFloat) {
    for (size_t i = 0; i < frames; ++i) {
        Sample* frame = buffer + i * mChannels;
        float* state = mState.data();
        for (int ch = 0; ch < mChannels; ++ch) {
            float x = static_cast<float>(frame[ch]) * toFloat * mPreamp;
            for (size_t b = 0; b < mBandCount; ++b, state += 2) {
                const Biquad& f = mFilters[b];
                const float y = f.b0 * x + state[0];
                state[0] = f.b1 * x - f.a1 * y + state[1];
                state[1] = f.b2 * x - f.a2 * y;
                x = y;
            }
            if constexpr (std::is_floating_point_v<Sample>) {
                frame[ch] = static_cast<Sample>(x);
            } else {
                // double, because INT32_MAX isn't representable as a float.
                const double lo = std::numeric_limits<Sample>::min();
                const double hi = std::numeric_limits<Sample>::max();
                frame[ch] = static_cast<Sample>(
                        std::clamp(static_cast<double>(x) * fromFloat, lo, hi));
            }
        }
    }
}

void CarEqualizer::process(void* buffer, size_t frames, const pcm_config& config) {
    if (!update(config.rate, static_cast<int>(config.channels))) return;

    switch (config.format) {
        case PCM_FORMAT_S16_LE:
            processInterleaved(static_cast<int16_t*>(buffer), frames, 1.0f / 32768.0f, 32768.0f);
            break;
        case PCM_FORMAT_S32_LE:
            processInterleaved(static_cast<int32_t*>(buffer), frames, 1.0f / 2147483648.0f,
                               2147483648.0f);
            break;
        case PCM_FORMAT_FLOAT_LE:
            processInterleaved(static_cast<float*>(buffer), frames, 1.0f, 1.0f);
            break;
        default:
            break;
    }
}

}  // namespace aidl::android::hardware::audio::core::primary
