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

#define LOG_TAG "AHAL_PrimaryMixer"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <regex>

#include <android-base/logging.h>
#include <android-base/properties.h>

#include "PrimaryMixer.h"

namespace aidl::android::hardware::audio::core::primary {

// static
int PrimaryMixer::findAlsaCardByName(const std::string& cardName) {
    // Parse /proc/asound/cards to find card by name
    // Format: " 0 [AMLAUGESOUND   ]: driver - longname"
    std::ifstream cardsFile("/proc/asound/cards");
    if (!cardsFile.is_open()) {
        LOG(WARNING) << __func__ << ": Cannot open /proc/asound/cards";
        return kInvalidAlsaCard;
    }

    std::string line;
    std::string cardNameLower = cardName;
    std::transform(cardNameLower.begin(), cardNameLower.end(), cardNameLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // Regex to match card line: whitespace, card number, space, [name], colon
    std::regex cardRegex(R"(^\s*(\d+)\s+\[([^\]]+)\])");

    while (std::getline(cardsFile, line)) {
        std::smatch match;
        if (std::regex_search(line, match, cardRegex)) {
            int cardNum = std::stoi(match[1].str());
            std::string name = match[2].str();
            // Trim trailing whitespace from name
            name.erase(name.find_last_not_of(" \t") + 1);

            std::string loweredName = name;
            std::transform(loweredName.begin(), loweredName.end(), loweredName.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            LOG(DEBUG) << __func__ << ": Found card " << cardNum << " name='" << name << "'";

            if (loweredName.find(cardNameLower) != std::string::npos) {
                LOG(INFO) << __func__ << ": Matched card " << cardNum
                          << " for requested name '" << cardName << "'";
                return cardNum;
            }
        }
    }

    LOG(WARNING) << __func__ << ": Card with name '" << cardName << "' not found";
    return kInvalidAlsaCard;
}

// static
int PrimaryMixer::getAlsaCard() {
    std::string cardName = ::android::base::GetProperty(
        "persist.vendor.audio.primary.card_name", "");

    if (!cardName.empty()) {
        int card = findAlsaCardByName(cardName);
        if (card != kInvalidAlsaCard) {
            LOG(INFO) << __func__ << ": Using ALSA card " << card
                      << " (found by name '" << cardName << "')";
            return card;
        }
        LOG(WARNING) << __func__ << ": Card name '" << cardName
                     << "' not found, falling back to card index";
    }
    return ::android::base::GetIntProperty("persist.vendor.audio.primary.card",
                                                kDefaultAlsaCard);
}

// static
int PrimaryMixer::getAlsaDevice() {
    // Read from system property, default to 0 if not set
    int device = ::android::base::GetIntProperty("persist.vendor.audio.primary.device",
                                                  kDefaultAlsaDevice);
    LOG(DEBUG) << __func__ << ": Using ALSA device " << device
               << " (from persist.vendor.audio.primary.device)";
    return device;
}

PrimaryMixer::PrimaryMixer() : alsa::Mixer(getAlsaCard()) {
    LOG(INFO) << "PrimaryMixer initialized with card=" << getAlsaCard()
              << " device=" << getAlsaDevice();
    loadCarSettings();
}

namespace {

const std::string kCarSettingPrefix = "persist.vendor.audio.car.";

float readCarSetting(const std::string& name, float min, float max) {
    const std::string value = ::android::base::GetProperty(kCarSettingPrefix + name, "");
    if (value.empty()) return 0.0f;
    char* end = nullptr;
    const float v = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(v)) return 0.0f;
    return std::clamp(v, min, max);
}

std::string eqBandSettingName(int band) {
    return "eq_band" + std::to_string(band);
}

// Band layout the stored eq_band<N> values belong to. Layout 1 (no property) was
// the 5-band EQ at 60/250/1k/4k/12k Hz; its values don't map onto the octave
// bands, so they are reset once.
constexpr float kEqLayout = 2.0f;

}  // namespace

void PrimaryMixer::loadCarSettings() {
    mBalance.store(readCarSetting("balance", -1.0f, 1.0f));
    mFader.store(readCarSetting("fader", -1.0f, 1.0f));
    mEqEnabled.store(readCarSetting("eq_enabled", 0.0f, 1.0f) != 0.0f);
    if (readCarSetting("eq_layout", 0.0f, 100.0f) != kEqLayout) {
        for (int band = 0; band < kEqBands; ++band) persistCarSetting(eqBandSettingName(band), 0.0f);
        persistCarSetting("eq_preamp", 0.0f);
        persistCarSetting("eq_layout", kEqLayout);
    }
    for (int band = 0; band < kEqBands; ++band) {
        mEqGainDb[band].store(readCarSetting(eqBandSettingName(band), -kEqMaxGainDb, kEqMaxGainDb));
    }
    mEqPreampDb.store(readCarSetting("eq_preamp", -kEqMaxGainDb, kEqMaxGainDb));
    LOG(INFO) << "Car audio settings: balance=" << mBalance.load() << " fader=" << mFader.load()
              << " eq=" << (mEqEnabled.load() ? "on" : "off");
}

// static
void PrimaryMixer::persistCarSetting(const std::string& name, float value) {
    if (!::android::base::SetProperty(kCarSettingPrefix + name, std::to_string(value))) {
        LOG(WARNING) << "Could not persist " << kCarSettingPrefix << name;
    }
}

// static
PrimaryMixer& PrimaryMixer::getInstance() {
    static PrimaryMixer gInstance;
    return gInstance;
}

// static
void PrimaryMixer::setBalance(float balance) {
    balance = std::clamp(balance, -1.0f, 1.0f);
    getInstance().mBalance.store(balance);
    persistCarSetting("balance", balance);
}

// static
void PrimaryMixer::setFader(float fader) {
    fader = std::clamp(fader, -1.0f, 1.0f);
    getInstance().mFader.store(fader);
    persistCarSetting("fader", fader);
}

// static
void PrimaryMixer::setEqEnabled(bool enabled) {
    auto& instance = getInstance();
    instance.mEqEnabled.store(enabled);
    instance.mEqGeneration.fetch_add(1);
    persistCarSetting("eq_enabled", enabled ? 1.0f : 0.0f);
}

// static
bool PrimaryMixer::getEqEnabled() {
    return getInstance().mEqEnabled.load();
}

// static
void PrimaryMixer::setEqBandGain(int band, float gainDb) {
    if (band < 0 || band >= kEqBands) return;
    gainDb = std::clamp(gainDb, -kEqMaxGainDb, kEqMaxGainDb);
    auto& instance = getInstance();
    instance.mEqGainDb[band].store(gainDb);
    instance.mEqGeneration.fetch_add(1);
    persistCarSetting(eqBandSettingName(band), gainDb);
}

// static
float PrimaryMixer::getEqBandGain(int band) {
    if (band < 0 || band >= kEqBands) return 0.0f;
    return getInstance().mEqGainDb[band].load();
}

// static
void PrimaryMixer::setEqPreamp(float gainDb) {
    gainDb = std::clamp(gainDb, -kEqMaxGainDb, kEqMaxGainDb);
    auto& instance = getInstance();
    instance.mEqPreampDb.store(gainDb);
    instance.mEqGeneration.fetch_add(1);
    persistCarSetting("eq_preamp", gainDb);
}

// static
float PrimaryMixer::getEqPreamp() {
    return getInstance().mEqPreampDb.load();
}

// static
uint32_t PrimaryMixer::getEqGeneration() {
    return getInstance().mEqGeneration.load();
}

// static
float PrimaryMixer::getBalance() {
    return getInstance().mBalance.load();
}

// static
float PrimaryMixer::getFader() {
    return getInstance().mFader.load();
}

// static
void PrimaryMixer::setBusGain(const std::string& address, float gainDb) {
    if (address.empty()) return;
    auto& instance = getInstance();
    std::lock_guard<std::mutex> lock(instance.mLock);
    instance.mBusGains[address] = gainDb;
    LOG(DEBUG) << "PrimaryMixer: Stored gain " << gainDb << "dB for bus " << address;
}

// static
float PrimaryMixer::getBusGain(const std::string& address) {
    if (address.empty()) return 0.0f;
    auto& instance = getInstance();
    std::lock_guard<std::mutex> lock(instance.mLock);
    auto it = instance.mBusGains.find(address);
    if (it != instance.mBusGains.end()) {
        return it->second;
    }
    return 0.0f; // Default 0dB
}

}  // namespace aidl::android::hardware::audio::core::primary
