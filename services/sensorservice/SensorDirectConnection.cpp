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

#include "SensorDirectConnection.h"
#include <android/util/ProtoOutputStream.h>
#include <frameworks/base/core/proto/android/service/sensor_service.proto.h>
#include <hardware/sensors.h>
#include "SensorDevice.h"

#define UNUSED(x) (void)(x)

namespace android {

using util::ProtoOutputStream;

SensorService::SensorDirectConnection::SensorDirectConnection(
        const sp<SensorService>& service, uid_t uid, pid_t pid, const sensors_direct_mem_t* mem,
        int32_t halChannelHandle, const String16& opPackageName, int deviceId)
      : mService(service),
        mUid(uid),
        mPid(pid),
        mMem(*mem),
        mHalChannelHandle(halChannelHandle),
        mOpPackageName(opPackageName),
        mDeviceId(deviceId),
        mDestroyed(false) {
    mUserId = multiuser_get_user_id(mUid);
    ALOGD_IF(DEBUG_CONNECTIONS, "Created SensorDirectConnection");
}

SensorService::SensorDirectConnection::~SensorDirectConnection() {
    ALOGD_IF(DEBUG_CONNECTIONS, "~SensorDirectConnection %p", this);
    destroy();
}

void SensorService::SensorDirectConnection::destroy() {
    Mutex::Autolock _l(mDestroyLock);
    // destroy once only
    if (mDestroyed) {
        return;
    }

    stopAll();
    mService->cleanupConnection(this);
    if (mMem.handle != nullptr) {
        native_handle_close_with_tag(mMem.handle);
        native_handle_delete(const_cast<struct native_handle*>(mMem.handle));
    }
    mDestroyed = true;
}

void SensorService::SensorDirectConnection::onFirstRef() {
}

void SensorService::SensorDirectConnection::dump(String8& result) const {
    Mutex::Autolock _l(mConnectionLock);
    result.appendFormat("\t%s | HAL channel handle %d | uid %d | pid %d\n",
                        String8(mOpPackageName).c_str(), getHalChannelHandle(), mUid, mPid);
    result.appendFormat("\tActivated sensor count: %zu\n", mActivated.size());
    dumpSensorInfoWithLock(result, mActivated);

    result.appendFormat("\tBackup sensor (opened but UID idle) count: %zu\n",
                        mActivatedBackup.size());
    dumpSensorInfoWithLock(result, mActivatedBackup);
}

void SensorService::SensorDirectConnection::dumpSensorInfoWithLock(
        String8& result, std::unordered_map<int, int> sensors) const {
    for (auto& i : sensors) {
        result.appendFormat("\t\t%s 0x%08x | rate %d\n", mService->getSensorName(i.first).c_str(),
                            i.first, i.second);
    }
}

/**
 * Dump debugging information as android.service.SensorDirectConnectionProto protobuf message using
 * ProtoOutputStream.
 *
 * See proto definition and some notes about ProtoOutputStream in
 * frameworks/base/core/proto/android/service/sensor_service.proto
 */
void SensorService::SensorDirectConnection::dump(ProtoOutputStream* proto) const {
    using namespace service::SensorDirectConnectionProto;
    Mutex::Autolock _l(mConnectionLock);
    proto->write(PACKAGE_NAME, std::string(String8(mOpPackageName).c_str()));
    proto->write(HAL_CHANNEL_HANDLE, getHalChannelHandle());
    proto->write(NUM_SENSOR_ACTIVATED, int(mActivated.size()));
    for (auto &i : mActivated) {
        uint64_t token = proto->start(SENSORS);
        proto->write(SensorProto::SENSOR, i.first);
        proto->write(SensorProto::RATE, i.second);
        proto->end(token);
    }
}

sp<BitTube> SensorService::SensorDirectConnection::getSensorChannel() const {
    return nullptr;
}

void SensorService::SensorDirectConnection::onSensorAccessChanged(bool hasAccess) {
    if (!hasAccess) {
        stopAll(true /* backupRecord */);
    } else {
        recoverAll();
    }
}

void SensorService::SensorDirectConnection::onMicSensorAccessChanged(bool isMicToggleOn) {
    // TODO(b/398253250): Create a more robust way to allow VNDK and system clients to be exempted
    if (mUid == AID_SYSTEM) {
        return;
    }

    if (isMicToggleOn) {
        capRates();
    } else {
        uncapRates();
    }
}

void SensorService::SensorDirectConnection::onMotionSensorAccessChanged() {
    if (mService->hasMotionSensorAccess(mUid)) {
        // This callback runs with SensorService's connection lock already held.
        if (mService->hasSensorAccessLocked(mUid, mOpPackageName)) recoverAll();
        return;
    }
    Mutex::Autolock lock(mConnectionLock);
    const sensors_direct_cfg_t stopConfig = {.rate_level = SENSOR_DIRECT_RATE_STOP};
    for (auto it = mActivated.begin(); it != mActivated.end();) {
        const auto sensor = mService->getSensorInterfaceFromHandle(it->first);
        if (sensor != nullptr && SensorService::isMotionSensor(sensor->getSensor().getType())) {
            configure(it->first, &stopConfig);
            mActivatedBackup[it->first] = it->second;
            it = mActivated.erase(it);
        } else {
            ++it;
        }
    }
}

bool SensorService::SensorDirectConnection::hasSensorAccess() const {
    return mService->hasSensorAccess(mUid, mOpPackageName);
}

status_t SensorService::SensorDirectConnection::enableDisable(
        int handle, bool enabled, nsecs_t samplingPeriodNs, nsecs_t maxBatchReportLatencyNs,
        int reservedFlags) {
    // SensorDirectConnection does not support enableDisable, parameters not used
    UNUSED(handle);
    UNUSED(enabled);
    UNUSED(samplingPeriodNs);
    UNUSED(maxBatchReportLatencyNs);
    UNUSED(reservedFlags);
    return INVALID_OPERATION;
}

status_t SensorService::SensorDirectConnection::setEventRate(
        int handle, nsecs_t samplingPeriodNs) {
    // SensorDirectConnection does not support setEventRate, parameters not used
    UNUSED(handle);
    UNUSED(samplingPeriodNs);
    return INVALID_OPERATION;
}

status_t SensorService::SensorDirectConnection::flush() {
    // SensorDirectConnection does not support flush
    return INVALID_OPERATION;
}

int32_t SensorService::SensorDirectConnection::configureChannel(int handle, int rateLevel) {

    if (handle == -1 && rateLevel == SENSOR_DIRECT_RATE_STOP) {
        stopAll();
        Mutex::Autolock lock(mConnectionLock);
        mActivatedBackup.clear();
        mMicRateBackup.clear();
        return NO_ERROR;
    }

    if (rateLevel != SENSOR_DIRECT_RATE_STOP && !hasSensorAccess()) {
        return PERMISSION_DENIED;
    }

    std::shared_ptr<SensorInterface> si = mService->getSensorInterfaceFromHandle(handle);
    if (si == nullptr) {
        return NAME_NOT_FOUND;
    }

    const Sensor& s = si->getSensor();
    if (rateLevel != SENSOR_DIRECT_RATE_STOP && SensorService::isMotionSensor(s.getType())
            && !mService->hasMotionSensorAccess(mUid)) {
        return PERMISSION_DENIED;
    }
    if (!mService->canAccessSensor(s, "config direct channel", mOpPackageName)) {
        return PERMISSION_DENIED;
    }

    if (s.getHighestDirectReportRateLevel() == 0
            || rateLevel > s.getHighestDirectReportRateLevel()
            || !s.isDirectChannelTypeSupported(mMem.type)) {
        return INVALID_OPERATION;
    }

    int requestedRateLevel = rateLevel;
    // TODO(b/398253250): Create a more robust way to allow VNDK and system clients to be exempted
    if (mService->isSensorInCappedSet(s.getType()) && rateLevel != SENSOR_DIRECT_RATE_STOP &&
        mUid != AID_SYSTEM) {
        status_t err = mService->adjustRateLevelBasedOnMicAndPermission(&rateLevel, mOpPackageName);
        if (err != OK) {
            return err;
        }
    }

    struct sensors_direct_cfg_t config = {
        .rate_level = rateLevel
    };

    Mutex::Autolock _l(mConnectionLock);
    int ret = configure(handle, &config);

    if (rateLevel == SENSOR_DIRECT_RATE_STOP) {
        if (ret == NO_ERROR) {
            mActivated.erase(handle);
            mActivatedBackup.erase(handle);
            mMicRateBackup.erase(handle);
        } else if (ret > 0) {
            ret = UNKNOWN_ERROR;
        }
    } else {
        if (ret > 0) {
            mActivated[handle] = rateLevel;
            if (mService->isSensorInCappedSet(s.getType())) {
                // Back up the rates that the app is allowed to have if the mic toggle is off
                // This is used in the uncapRates() function.
                if ((requestedRateLevel <= SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL) ||
                    !isRateCappedBasedOnPermission()) {
                    mMicRateBackup[handle] = requestedRateLevel;
                } else {
                    mMicRateBackup[handle] = SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL;
                }
            }
        }
    }

    return ret;
}

void SensorService::SensorDirectConnection::capRates() {
    Mutex::Autolock _l(mConnectionLock);
    const struct sensors_direct_cfg_t capConfig = {
        .rate_level = SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL
    };

    const struct sensors_direct_cfg_t stopConfig = {
        .rate_level = SENSOR_DIRECT_RATE_STOP
    };

    // Motion privacy can stop a subset of a channel's sensors. Update both sets of rates.
    const auto cap = [&](std::unordered_map<int, int>& connections, bool active) {
        for (auto &i : connections) {
            const int handle = i.first;
            const int rateLevel = i.second;
            const auto si = mService->getSensorInterfaceFromHandle(handle);
            if (si != nullptr && mService->isSensorInCappedSet(si->getSensor().getType())
                    && rateLevel > SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL) {
                mMicRateBackup[handle] = rateLevel;
                i.second = SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL;
                if (active) {
                    configure(handle, &stopConfig);
                    configure(handle, &capConfig);
                }
            }
        }
    };
    cap(mActivated, true);
    cap(mActivatedBackup, false);
}

void SensorService::SensorDirectConnection::uncapRates() {
    Mutex::Autolock _l(mConnectionLock);

    const struct sensors_direct_cfg_t stopConfig = {
        .rate_level = SENSOR_DIRECT_RATE_STOP
    };
    for (auto &i : mMicRateBackup) {
        int handle = i.first;
        int rateLevel = i.second;

        const struct sensors_direct_cfg_t config = {
            .rate_level = rateLevel
        };

        // Modify the rate kept by the existing map
        const auto active = mActivated.find(handle);
        const auto stopped = mActivatedBackup.find(handle);
        if (active != mActivated.end()) active->second = rateLevel;
        else if (stopped != mActivatedBackup.end()) stopped->second = rateLevel;
        else continue;

        // Only reconfigure the channel if it's ongoing
        if (active != mActivated.end()) {
            // Stopping before reconfiguring is the well-tested path in CTS
            configure(handle, &stopConfig);
            configure(handle, &config);
        }
    }
    mMicRateBackup.clear();
}

int SensorService::SensorDirectConnection::configure(
        int handle, const sensors_direct_cfg_t* config) {
    if (mDeviceId == RuntimeSensor::DEFAULT_DEVICE_ID) {
        SensorDevice& dev(SensorDevice::getInstance());
        return dev.configureDirectChannel(handle, getHalChannelHandle(), config);
    } else {
        return mService->configureRuntimeSensorDirectChannel(handle, this, config);
    }
}

void SensorService::SensorDirectConnection::stopAll(bool backupRecord) {
    Mutex::Autolock _l(mConnectionLock);
    stopAllLocked(backupRecord);
}

void SensorService::SensorDirectConnection::stopAllLocked(bool backupRecord) {
    struct sensors_direct_cfg_t config = {
        .rate_level = SENSOR_DIRECT_RATE_STOP
    };

    for (auto &i : mActivated) {
        configure(i.first, &config);
    }

    if (backupRecord) {
        mActivatedBackup.insert(mActivated.begin(), mActivated.end());
    }
    mActivated.clear();
}

void SensorService::SensorDirectConnection::recoverAll() {
    Mutex::Autolock _l(mConnectionLock);
    for (auto it = mActivatedBackup.begin(); it != mActivatedBackup.end();) {
        const auto sensor = mService->getSensorInterfaceFromHandle(it->first);
        if (sensor != nullptr && SensorService::isMotionSensor(sensor->getSensor().getType())
                && !mService->hasMotionSensorAccess(mUid)) {
            ++it;
            continue;
        }
        const sensors_direct_cfg_t config = {.rate_level = it->second};
        if (configure(it->first, &config) > 0) {
            mActivated[it->first] = it->second;
            it = mActivatedBackup.erase(it);
        } else {
            ++it;
        }
    }
}

int32_t SensorService::SensorDirectConnection::getHalChannelHandle() const {
    return mHalChannelHandle;
}

bool SensorService::SensorDirectConnection::isEquivalent(const sensors_direct_mem_t *mem) const {
    bool ret = false;

    if (mMem.type == mem->type) {
        switch (mMem.type) {
            case SENSOR_DIRECT_MEM_TYPE_ASHMEM: {
                // there is no known method to test if two ashmem fds are equivalent besides
                // trivially comparing the fd values (ino number from fstat() are always the
                // same, pointing to "/dev/ashmem").
                int fd1 = mMem.handle->data[0];
                int fd2 = mem->handle->data[0];
                ret = (fd1 == fd2);
                break;
            }
            case SENSOR_DIRECT_MEM_TYPE_GRALLOC:
                // there is no known method to test if two gralloc handle are equivalent
                ret = false;
                break;
            default:
                // should never happen
                ALOGE("Unexpected mem type %d", mMem.type);
                ret = true;
                break;
        }
    }
    return ret;
}

} // namespace android

