/* Copyright (C) 2026 The ArkUI Project; SPDX-License-Identifier: Apache-2.0 */
package android.hardware;

oneway interface IMotionSensorPrivacyListener {
    void onMotionSensorPrivacyChanged(int uid, long blockedUntil);
}
