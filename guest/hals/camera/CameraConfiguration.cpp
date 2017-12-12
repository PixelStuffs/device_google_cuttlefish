/*
 * Copyright (C) 2016 The Android Open Source Project
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
#include "GceDevicePersonalityImpl.h"

#define LOG_TAG "GceDevicePersonality"

#include <android-base/file.h>
#include <android-base/strings.h>
#include <cutils/log.h>
#include <json/json.h>
#include <json/reader.h>
#include <GceMetadataAttributes.h>
#include <GceResourceLocation.h>
#include <stdlib.h>

namespace avd {
namespace {
////////////////////// Device Personality keys //////////////////////
//
// **** Camera ****
//
// Example segment (transcribed to constants):
//
// kCameraDefinitionsKey: [
//   {
//     kCameraDefinitionOrientationKey: "front",
//     kCameraDefinitionHalVersionKey: "1",
//     kCameraDefinitionResolutionsKey: [
//       {
//         kCameraDefinitionResolutionWidthKey: "1600",
//         kCameraDefinitionResolutionHeightKey: "1200",
//       },
//       {
//         kCameraDefinitionResolutionWidthKey: "1280",
//         kCameraDefinitionResolutionHeightKey: "800",
//       }
//     ]
//   },
//   {
//     kCameraDefinitionOrientationKey: "back",
//     kCameraDefinitionHalVersionKey: "1",
//     kCameraDefinitionResolutionsKey: [
//       {
//         kCameraDefinitionResolutionWidthKey: "1024",
//         kCameraDefinitionResolutionHeightKey: "768",
//       },
//       {
//         kCameraDefinitionResolutionWidthKey: "800",
//         kCameraDefinitionResolutionHeightKey: "600",
//       }
//     ]
//   }
// ]
//
//
// Array of camera definitions for all cameras available on the device (array).
// Top Level Key.
const char* const kCameraDefinitionsKey = "camera_definitions";

// Camera orientation of currently defined camera (string).
// Currently supported values:
// - "back",
// - "front".
const char* const kCameraDefinitionOrientationKey = "orientation";

// Camera HAL version of currently defined camera (int).
// Currently supported values:
// - 1 (Camera HALv1)
// - 2 (Camera HALv2)
// - 3 (Camera HALv3)
const char* const kCameraDefinitionHalVersionKey = "hal_version";

// Array of resolutions supported by camera (array).
const char* const kCameraDefinitionResolutionsKey = "resolutions";

// Width of currently defined resolution (int).
// Must be divisible by 8.
const char* const kCameraDefinitionResolutionWidthKey = "width";

// Height of currently defined resolution (int).
// Must be divisible by 8.
const char* const kCameraDefinitionResolutionHeightKey = "height";

// Convert string value to camera orientation.
bool ValueToCameraOrientation(
    const std::string& value,
    personality::Camera::Orientation* orientation) {
  if (value == "back") {
    *orientation = personality::Camera::kBack;
    return true;
  } else if (value == "front") {
    *orientation = personality::Camera::kFront;
    return true;
  }
  ALOGE("%s: Invalid camera orientation: %s.",
        __FUNCTION__, value.c_str());
  return false;
}

// Convert string value to camera HAL version.
bool ValueToCameraHalVersion(
    const std::string& value,
    personality::Camera::HalVersion* hal_version) {
  int temp;
  char* endptr;

  temp = strtol(value.c_str(), &endptr, 10);
  if (endptr != value.c_str() + value.size()) {
    ALOGE("%s: Invalid camera HAL version. Expected number, got %s.",
          __FUNCTION__, value.c_str());
    return false;
  }

  switch (temp) {
    case 1:
      *hal_version = personality::Camera::kHalV1;
      break;

    case 2:
      *hal_version = personality::Camera::kHalV2;
      break;

    case 3:
      *hal_version = personality::Camera::kHalV3;
      break;

    default:
      ALOGE("%s: Invalid camera HAL version. Version %d not supported.",
            __FUNCTION__, temp);
      return false;
  }

  return true;
}

bool ValueToCameraResolution(
    const std::string& width, const std::string& height,
    personality::Camera::Resolution* resolution) {
  char* endptr;

  resolution->width = strtol(width.c_str(), &endptr, 10);
  if (endptr != width.c_str() + width.size()) {
    ALOGE("%s: Invalid camera resolution width. Expected number, got %s.",
          __FUNCTION__, width.c_str());
    return false;
  }

  resolution->height = strtol(height.c_str(), &endptr, 10);
  if (endptr != height.c_str() + height.size()) {
    ALOGE("%s: Invalid camera resolution height. Expected number, got %s.",
          __FUNCTION__, height.c_str());
    return false;
  }

  // Validate width and height parameters are sane.
  if (resolution->width <= 0 || resolution->height <= 0) {
    ALOGE("%s: Invalid camera resolution: %dx%d", __FUNCTION__,
          resolution->width, resolution->height);
    return false;
  }

  // Validate width and height divisible by 8.
  if ((resolution->width & 7) != 0 || (resolution->height & 7) != 0) {
    ALOGE("%s: Invalid camera resolution: width and height must be "
          "divisible by 8, got %dx%d (%dx%d).", __FUNCTION__,
          resolution->width, resolution->height,
          resolution->width & 7, resolution->height & 7);
    return false;
  }

  return true;
}

// Process camera definitions.
// Returns true, if definitions were sane.
bool ConfigureCameras(
    const Json::Value& value,
    std::vector<personality::Camera>* cameras) {
  if (!value.isObject()) {
    ALOGE("%s: Personality root is not an object", __FUNCTION__);
    return false;
  }

  if (!value.isMember(kCameraDefinitionsKey)) return true;
  for (Json::ValueConstIterator iter = value[kCameraDefinitionsKey].begin();
       iter != value[kCameraDefinitionsKey].end();
       ++iter) {
    cameras->push_back(personality::Camera());
    personality::Camera& camera = cameras->back();

    if (!iter->isObject()) {
      ALOGE("%s: Camera definition is not an object", __FUNCTION__);
      continue;
    }

    // Camera without orientation -> invalid setting.
    if (!iter->isMember(kCameraDefinitionOrientationKey)) {
      ALOGE("%s: Invalid camera definition: key %s is missing.",
            __FUNCTION__, kCameraDefinitionOrientationKey);
      return false;
    }

    if (!ValueToCameraOrientation(
        (*iter)[kCameraDefinitionOrientationKey].asString(),
        &camera.orientation)) return false;

    // Camera without HAL version -> invalid setting.
    if (!(*iter).isMember(kCameraDefinitionHalVersionKey)) {
      ALOGE("%s: Invalid camera definition: key %s is missing.",
            __FUNCTION__, kCameraDefinitionHalVersionKey);
      return false;
    }

    if (!ValueToCameraHalVersion(
        (*iter)[kCameraDefinitionHalVersionKey].asString(),
        &camera.hal_version)) return false;

    // Camera without resolutions -> invalid setting.
    if (!iter->isMember(kCameraDefinitionResolutionsKey)) {
      ALOGE("%s: Invalid camera definition: key %s is missing.",
            __FUNCTION__, kCameraDefinitionResolutionsKey);
      return false;
    }

    const Json::Value& json_resolutions =
        (*iter)[kCameraDefinitionResolutionsKey];

    // Resolutions not an array, or an empty array -> invalid setting.
    if (!json_resolutions.isArray() || json_resolutions.empty()) {
      ALOGE("%s: Invalid camera definition: %s is not an array or is empty.",
            __FUNCTION__, kCameraDefinitionResolutionsKey);
      return false;
    }

    // Process all resolutions.
    for (Json::ValueConstIterator json_res_iter = json_resolutions.begin();
         json_res_iter != json_resolutions.end();
         ++json_res_iter) {
      // Check presence of width and height keys.
      if (!json_res_iter->isObject()) {
        ALOGE("%s: Camera resolution item is not an object", __FUNCTION__);
        continue;
      }
      if (!json_res_iter->isMember(kCameraDefinitionResolutionWidthKey) ||
          !json_res_iter->isMember(kCameraDefinitionResolutionHeightKey)) {
        ALOGE("%s: Invalid camera resolution: keys %s and %s are both required.",
              __FUNCTION__,
              kCameraDefinitionResolutionWidthKey,
              kCameraDefinitionResolutionHeightKey);
        return false;
      }

      camera.resolutions.push_back(personality::Camera::Resolution());
      personality::Camera::Resolution& resolution = camera.resolutions.back();

      if (!ValueToCameraResolution(
          (*json_res_iter)[kCameraDefinitionResolutionWidthKey].asString(),
          (*json_res_iter)[kCameraDefinitionResolutionHeightKey].asString(),
          &resolution)) return false;
    }
  }

  return true;
}

bool ConfigureCameraFromLegacySetting(
    const std::string& setting,
    personality::Camera* camera) {
  std::stringstream prop_reader(setting);
  std::string hal_version;
  if (!std::getline(prop_reader, hal_version, ',')) return false;
  if (!ValueToCameraHalVersion(hal_version, &camera->hal_version)) return false;

  std::string width, height;
  if (!std::getline(prop_reader, width, ',')) return false;
  if (!std::getline(prop_reader, height, ',')) return false;

  camera->resolutions.push_back(personality::Camera::Resolution());
  if (!ValueToCameraResolution(width, height,
                               &camera->resolutions.back())) return false;

  // Final component: style. Ignore.

  return true;
}

bool ConfigureCamerasFromLegacySettings(
    InitialMetadataReader* reader,
    std::vector<personality::Camera>* cameras) {
  const char* value = reader->GetValueForKey(
      GceMetadataAttributes::kFrontCameraConfigKey);
  if (value) {
    personality::Camera camera;
    camera.orientation = personality::Camera::kFront;
    if (ConfigureCameraFromLegacySetting(value, &camera)) {
      cameras->push_back(camera);
    }
  }

  value = reader->GetValueForKey(
      GceMetadataAttributes::kBackCameraConfigKey);
  if (value) {
    personality::Camera camera;
    camera.orientation = personality::Camera::kFront;
    if (ConfigureCameraFromLegacySetting(value, &camera)) {
      cameras->push_back(camera);
    }
  }

  return true;
}

}  // namespace

void GceDevicePersonalityImpl::Init() {
  // Try parsing user supplied JSON.
  Reset();
  const char* personality = reader_->GetValueForKey(
      GceMetadataAttributes::kDevicePersonalityDefinitionKey);
  if (personality && InitFromJsonObject(personality)) {
    return;
  }

  // Try parsing file selected by User.
  Reset();
  personality = reader_->GetValueForKey(
      GceMetadataAttributes::kDevicePersonalityNameKey);
  if (personality && InitFromPersonalityName(personality)) {
    return;
  }

  // Fallback: Init from default file.
  Reset();
  if (InitFromPersonalityName("default")) {
    return;
  }

  ALOGE("%s: Could not initialize device personality from any source.",
        __FUNCTION__);

  Reset();
  InitFromLegacySettings();
}

void GceDevicePersonalityImpl::Reset() {
  cameras_.clear();
}

bool GceDevicePersonalityImpl::InitFromJsonObject(
    const std::string& json_object) {
  Json::Reader personality_reader;
  Json::Value root;
  if (!personality_reader.parse(json_object, root)) {
    ALOGE("%s: Could not parse personality: %s", __FUNCTION__,
          personality_reader.getFormattedErrorMessages().c_str());
    return false;
  }

  return ConfigureCameras(root, &cameras_);
}

bool GceDevicePersonalityImpl::InitFromPersonalityName(
    const std::string& personality_name) {
  std::string path(GceResourceLocation::kDevicePersonalitiesPath);
  path += "/";
  path += personality_name;
  path += ".json";

  std::string personality;
  if (!android::base::ReadFileToString(path, &personality)) {
    ALOGE("%s: Could not open personality file: %s",
          __FUNCTION__, path.c_str());
    return false;
  }

  ALOGI("%s: Parsing personality file: %s",
        __FUNCTION__, personality_name.c_str());

  return InitFromJsonObject(personality);
}

bool GceDevicePersonalityImpl::InitFromLegacySettings() {
  return ConfigureCamerasFromLegacySettings(reader_, &cameras_);
}

GceDevicePersonality* GceDevicePersonality::getInstance(
    InitialMetadataReader* reader) {
  static GceDevicePersonalityImpl* instance;
  if (!instance) {
    instance = new GceDevicePersonalityImpl(reader);
    instance->Init();
  }
  return instance;
}

}  // namespace avd


