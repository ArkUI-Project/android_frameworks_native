/*
 * Copyright 2022 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "LayerSettingsTest"

#include <gtest/gtest.h>
#include <renderengine/LayerSettings.h>

namespace android::renderengine {

TEST(LayerSettingsTest, whitePointNits) {
    LayerSettings a, b;
    ASSERT_EQ(a, b);

    a.whitePointNits = 45.f;

    ASSERT_FALSE(a == b);
}

TEST(LayerSettingsTest, progressiveBlurChangesCompositionIdentity) {
    LayerSettings a;
    a.blurRegions.push_back(BlurRegion{40, 0, 0, 0, 0, 1, 0, 0, 100, 200});
    auto b = a;
    b.blurRegions[0].blurRadius |= BlurRegion::kProgressive;
    EXPECT_FALSE(a == b);
    EXPECT_NE(std::hash<BlurRegion>{}(a.blurRegions[0]),
              std::hash<BlurRegion>{}(b.blurRegions[0]));
    EXPECT_EQ(40u, b.blurRegions[0].radius());
    EXPECT_TRUE(b.blurRegions[0].isProgressive());
    EXPECT_FALSE(a.blurRegions[0].isProgressive());
    EXPECT_EQ(40u, sizeof(BlurRegion));
}
} // namespace android::renderengine
