/*
 * Copyright (C) 2018 The Android Open Source Project
 * Copyright (C) 2020 The LineageOS Project
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

#define LOG_TAG "android.hardware.light@2.0-service.xiaomi_pyxis"

#include "Light.h"

#include <android-base/file.h>
#include <android-base/logging.h>

#include <iomanip>

namespace {

#define LEDS "/sys/class/leds/"
#define LCD_LED "/sys/class/backlight/panel0-backlight/"
#define RED LEDS "red/"
#define BLUE LEDS "blue/"
#define GREEN LEDS "green/"

#define BRIGHTNESS "brightness"
#define MAX_BRIGHTNESS "max_brightness"

#define BLINK "blink"
#define DUTY_PCTS "duty_pcts"
#define PAUSE_HI "pause_hi"
#define PAUSE_LO "pause_lo"
#define RAMP_STEP_MS "ramp_step_ms"
#define START_IDX "start_idx"
#define LUT_FLAGS "lut_flags"

using ::android::base::ReadFileToString;
using ::android::base::WriteStringToFile;

// Default max brightness
constexpr auto kDefaultMaxLedBrightness = 255;
constexpr auto kDefaultMaxScreenBrightness = 2047;

// Each step will stay on for 150ms by default.
constexpr auto kRampStepDuration = 150;

// Each value represents a duty percent (0 - 100) for the led pwm.
constexpr std::array kBrightnessRamp = {0, 12, 25, 37, 50, 72, 85, 100};

// Write value to path and close file.
bool WriteToFile(const std::string& path, uint32_t content) {
    return WriteStringToFile(std::to_string(content), path);
}

bool WriteToFile(const std::string& path, const std::string& content) {
    return WriteStringToFile(content, path);
}

uint32_t RgbaToBrightness(uint32_t color) {
    // Extract brightness from AARRGGBB.
    uint32_t alpha = (color >> 24) & 0xFF;

    // Retrieve each of the RGB colors
    uint32_t red = (color >> 16) & 0xFF;
    uint32_t blue = color & 0xFF;
    uint32_t green = (color >> 8) & 0xFF;

    // Scale RGB colors if a brightness has been applied by the user
    if (alpha != 0xFF) {
        red = red * alpha / 0xFF;
        blue = blue * alpha / 0xFF;
        green = green * alpha / 0xFF;
    }

    return (77 * red + 150 * green + 29 * blue) >> 8;
}

inline uint32_t RgbaToBrightness(uint32_t color, uint32_t max_brightness) {
    return RgbaToBrightness(color) * max_brightness / 0xFF;
}

/*
 * Scale each value of the brightness ramp according to the
 * brightness of the color.
 */
std::string GetScaledDutyPcts(uint32_t brightness) {
    std::stringstream ramp;

    for (size_t i = 0; i < kBrightnessRamp.size(); i++) {
        if (i > 0) ramp << ",";
        ramp << kBrightnessRamp[i] * brightness / 0xFF;
    }

    return ramp.str();
}

inline bool IsLit(uint32_t color) {
    return color & 0x00ffffff;
}

}  // anonymous namespace

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

Light::Light() {
    std::string buf;

    if (ReadFileToString(LCD_LED MAX_BRIGHTNESS, &buf)) {
        max_screen_brightness_ = std::stoi(buf);
    } else {
        max_screen_brightness_ = kDefaultMaxScreenBrightness;
        LOG(ERROR) << "Failed to read max screen brightness, fallback to "
                   << kDefaultMaxLedBrightness;
    }

    if (ReadFileToString(RED MAX_BRIGHTNESS, &buf)) {
        max_red_led_brightness_ = std::stoi(buf);
    } else {
        max_red_led_brightness_ = kDefaultMaxLedBrightness;
        LOG(ERROR) << "Failed to read max red LED brightness, fallback to " << kDefaultMaxLedBrightness;
    }
    if (ReadFileToString(BLUE MAX_BRIGHTNESS, &buf)) {
        max_blue_led_brightness_= std::stoi(buf);
    } else {
        max_blue_led_brightness_ = kDefaultMaxLedBrightness;
        LOG(ERROR) << "Failed to read max blue LED brightness, fallback to " << kDefaultMaxLedBrightness;
    }

    if (ReadFileToString(GREEN MAX_BRIGHTNESS, &buf)) {
        max_green_led_brightness_= std::stoi(buf);
    } else {
        max_green_led_brightness_ = kDefaultMaxLedBrightness;
        LOG(ERROR) << "Failed to read max green LED brightness, fallback to " << kDefaultMaxLedBrightness;
    }
}

Return<Status> Light::setLight(Type type, const LightState& state) {
    auto it = lights_.find(type);

    if (it == lights_.end()) {
        return Status::LIGHT_NOT_SUPPORTED;
    }

    it->second(type, state);

    return Status::SUCCESS;
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    std::vector<Type> types;

    for (auto&& light : lights_) types.emplace_back(light.first);

    _hidl_cb(types);

    return Void();
}

void Light::setLightBacklight(Type /*type*/, const LightState& state) {
    uint32_t brightness = RgbaToBrightness(state.color, max_screen_brightness_);
    WriteToFile(LCD_LED BRIGHTNESS, brightness);
}

void Light::setLightNotification(Type type, const LightState& state) {
    bool found = false;
    for (auto&& [cur_type, cur_state] : notif_states_) {
        if (cur_type == type) {
            cur_state = state;
        }

        // Fallback to battery light
        if (!found && (cur_type == Type::BATTERY || IsLit(state.color))) {
            found = true;
            LOG(DEBUG) << __func__ << ": type=" << toString(cur_type);
            applyNotificationState(state);
        }
    }
}

void Light::applyNotificationState(const LightState& state) {
    uint32_t red_brightness = RgbaToBrightness(state.color, max_red_led_brightness_);
    uint32_t blue_brightness = RgbaToBrightness(state.color, max_blue_led_brightness_);
    uint32_t green_brightness = RgbaToBrightness(state.color, max_green_led_brightness_);

    // Turn off the leds (initially)
    WriteToFile(RED BLINK, 0);
    WriteToFile(BLUE BLINK, 0);
    WriteToFile(GREEN BLINK, 0);

    if (state.flashMode == Flash::TIMED && state.flashOnMs > 0 && state.flashOffMs > 0) {
        /*
         * If the flashOnMs duration is not long enough to fit ramping up
         * and down at the default step duration, step duration is modified
         * to fit.
         */
        int32_t step_duration = kRampStepDuration;
        int32_t pause_hi = state.flashOnMs - (step_duration * kBrightnessRamp.size() * 2);
        if (pause_hi < 0) {
            step_duration = state.flashOnMs / (kBrightnessRamp.size() * 2);
            pause_hi = 0;
        }

        LOG(DEBUG) << __func__ << ": color=" << std::hex << state.color << std::dec
                   << " onMs=" << state.flashOnMs << " offMs=" << state.flashOffMs;

        // RED
        WriteToFile(RED LUT_FLAGS, 95);
        WriteToFile(RED START_IDX, 0);
        WriteToFile(RED DUTY_PCTS, GetScaledDutyPcts(red_brightness));
        WriteToFile(RED PAUSE_LO, static_cast<uint32_t>(state.flashOffMs));
        WriteToFile(RED PAUSE_HI, static_cast<uint32_t>(pause_hi));
        WriteToFile(RED RAMP_STEP_MS, static_cast<uint32_t>(step_duration));
        WriteToFile(RED BLINK, 1);

        // BLUE
        WriteToFile(BLUE LUT_FLAGS, 95);
        WriteToFile(BLUE START_IDX, 0);
        WriteToFile(BLUE DUTY_PCTS, GetScaledDutyPcts(blue_brightness));
        WriteToFile(BLUE PAUSE_LO, static_cast<uint32_t>(state.flashOffMs));
        WriteToFile(BLUE PAUSE_HI, static_cast<uint32_t>(pause_hi));
        WriteToFile(BLUE RAMP_STEP_MS, static_cast<uint32_t>(step_duration));
        WriteToFile(BLUE BLINK, 1);

        // GREEN
        WriteToFile(GREEN LUT_FLAGS, 95);
        WriteToFile(GREEN START_IDX, 0);
        WriteToFile(GREEN DUTY_PCTS, GetScaledDutyPcts(green_brightness));
        WriteToFile(GREEN PAUSE_LO, static_cast<uint32_t>(state.flashOffMs));
        WriteToFile(GREEN PAUSE_HI, static_cast<uint32_t>(pause_hi));
        WriteToFile(GREEN RAMP_STEP_MS, static_cast<uint32_t>(step_duration));
        WriteToFile(GREEN BLINK, 1);
    } else {
        WriteToFile(RED BRIGHTNESS, red_brightness);
        WriteToFile(BLUE BRIGHTNESS, blue_brightness);
        WriteToFile(GREEN BRIGHTNESS, green_brightness);
    }
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
