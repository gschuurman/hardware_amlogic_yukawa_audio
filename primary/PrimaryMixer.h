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

#pragma once

#include <atomic>
#include <string>

#include "alsa/Mixer.h"

namespace aidl::android::hardware::audio::core::primary {

class PrimaryMixer : public alsa::Mixer {
  public:
    static constexpr int kInvalidAlsaCard = -1;
    static constexpr int kInvalidAlsaDevice = -1;
    static constexpr int kDefaultAlsaCard = 0;
    static constexpr int kDefaultAlsaDevice = 0;

    static PrimaryMixer& getInstance();
    static int getAlsaCard();
    static int getAlsaDevice();
    static int findAlsaCardByName(const std::string& cardName);

    // Automotive balance/fader controls.
    // balance: -1.0 = full left, 0.0 = center, +1.0 = full right
    // fader:   -1.0 = full rear, 0.0 = center, +1.0 = full front
    static void setBalance(float balance);
    static void setFader(float fader);
    static float getBalance();
    static float getFader();

  private:
    PrimaryMixer();

    std::atomic<float> mBalance{0.0f};
    std::atomic<float> mFader{0.0f};
};

}  // namespace aidl::android::hardware::audio::core::primary
