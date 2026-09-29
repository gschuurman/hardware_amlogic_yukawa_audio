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

#pragma once

#include <array>
#include <atomic>
#include <map>
#include <mutex>
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

    // Automotive equalizer: kEqBands peaking bands at kEqFrequenciesHz, gains
    // in dB within +/-kEqMaxGainDb. Applied per stream by CarEqualizer.
    static constexpr int kEqBands = 5;
    static constexpr std::array<float, kEqBands> kEqFrequenciesHz = {60.0f, 250.0f, 1000.0f,
                                                                      4000.0f, 12000.0f};
    static constexpr float kEqMaxGainDb = 12.0f;
    static void setEqEnabled(bool enabled);
    static bool getEqEnabled();
    static void setEqBandGain(int band, float gainDb);
    static float getEqBandGain(int band);
    // Incremented on every EQ change, so streams know when to recompute filters.
    static uint32_t getEqGeneration();

    // Automotive Bus Volume controls
    // Sets the gain in Decibels (dB) for a specific bus address (e.g., "bus0_media_out")
    static void setBusGain(const std::string& address, float gainDb);
    static float getBusGain(const std::string& address);

  private:
    PrimaryMixer();

    // Balance, fader and EQ are kept in persist.vendor.audio.car.* so they
    // survive a reboot and apply from the first sound on.
    void loadCarSettings();
    static void persistCarSetting(const std::string& name, float value);

    std::atomic<float> mBalance{0.0f};
    std::atomic<float> mFader{0.0f};
    std::atomic<bool> mEqEnabled{false};
    std::array<std::atomic<float>, kEqBands> mEqGainDb{};
    std::atomic<uint32_t> mEqGeneration{0};

    std::mutex mLock;
    std::map<std::string, float> mBusGains; // Address -> Gain in dB
};

}  // namespace aidl::android::hardware::audio::core::primary