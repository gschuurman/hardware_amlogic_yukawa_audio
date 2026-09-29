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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <tinyalsa/asoundlib.h>

#include "primary/PrimaryMixer.h"

namespace aidl::android::hardware::audio::core::primary {

// Automotive equalizer for one output stream: a chain of peaking biquads
// (one per PrimaryMixer EQ band) applied to every channel. The settings live in
// PrimaryMixer; the filters are recomputed whenever they change.
class CarEqualizer {
  public:
    // Filters an interleaved buffer in place.
    void process(void* buffer, size_t frames, const pcm_config& config);

    // Frequency response of the current settings at 'frequencyHz', in dB.
    // Used by the host-side test.
    float responseDb(float frequencyHz) const;

    // Recomputes the filters for the given format if the settings changed.
    // Returns false when the equalizer is disabled or flat.
    bool update(unsigned int sampleRate, int channels);

  private:
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    };

    template <typename Sample>
    void processInterleaved(Sample* buffer, size_t frames, float toFloat, float fromFloat);

    uint32_t mGeneration = UINT32_MAX;
    unsigned int mSampleRate = 0;
    int mChannels = 0;
    bool mActive = false;
    float mPreamp = 1.0f;
    size_t mBandCount = 0;
    std::array<Biquad, PrimaryMixer::kEqBands> mFilters;
    // Transposed direct form II state: [channel][band][2].
    std::vector<float> mState;
};

}  // namespace aidl::android::hardware::audio::core::primary
