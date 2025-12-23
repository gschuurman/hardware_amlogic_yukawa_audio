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
    // First, try to find card by name if specified
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

    // Fall back to card index from property
    int card = ::android::base::GetIntProperty("persist.vendor.audio.primary.card",
                                                kDefaultAlsaCard);
    LOG(DEBUG) << __func__ << ": Using ALSA card " << card
               << " (from persist.vendor.audio.primary.card)";
    return card;
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
}

// static
PrimaryMixer& PrimaryMixer::getInstance() {
    static PrimaryMixer gInstance;
    return gInstance;
}

// static
void PrimaryMixer::setBalance(float balance) {
    getInstance().mBalance.store(std::clamp(balance, -1.0f, 1.0f));
}

// static
void PrimaryMixer::setFader(float fader) {
    getInstance().mFader.store(std::clamp(fader, -1.0f, 1.0f));
}

// static
float PrimaryMixer::getBalance() {
    return getInstance().mBalance.load();
}

// static
float PrimaryMixer::getFader() {
    return getInstance().mFader.load();
}

}  // namespace aidl::android::hardware::audio::core::primary
