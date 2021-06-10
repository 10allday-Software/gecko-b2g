/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HAL_GONK_GONKSENSORSHAL_H_
#define HAL_GONK_GONKSENSORSHAL_H_

#include "base/thread.h"
#include "HalSensor.h"

#include "android/hardware/sensors/1.0/types.h"
#include "android/hardware/sensors/2.0/types.h"
#include "android_sensors/ISensorsWrapper.h"
#include "fmq/MessageQueue.h"

using namespace mozilla::hal;
using namespace android::hardware::sensors;
using namespace android::SensorServiceUtil;
namespace hidl_sensors = android::hardware::sensors::V1_0;
using hidl_sensors::SensorFlagBits;
using android::hardware::Void;
using android::hardware::hidl_vec;
using android::hardware::kSynchronizedReadWrite;
using android::hardware::MessageQueue;
using android::hardware::EventFlag;
using android::hardware::sensors::V2_0::EventQueueFlagBits;

#define MAX_EVENT_BUFFER_SIZE 16

namespace mozilla {
namespace hal_impl {

typedef void (* SensorDataCallback)(const SensorData& aSensorData);

class GonkSensorsHal {
public:
  static GonkSensorsHal* GetInstance() {
    if (sInstance == nullptr) {
      sInstance = new GonkSensorsHal();
    }
    return sInstance;
  };

  bool RegisterSensorDataCallback(const SensorDataCallback aCallback);
  bool ActivateSensor(const SensorType aSensorType);
  bool DeactivateSensor(const SensorType aSensorType);
private:
  class SensorDataNotifier;

  static GonkSensorsHal* sInstance;

  GonkSensorsHal()
    : mSensors(nullptr),
      mPollingThread(nullptr),
      mSensorDataCallback(nullptr),
      mEventQueueFlag(nullptr) {
        memset(mSensorInfoList, 0, sizeof(mSensorInfoList));
        Init();
  };
  ~GonkSensorsHal() {};

  void Init();
  bool InitHidlService();
  bool InitHidlServiceV1_0(android::sp<V1_0::ISensors> aServiceV1_0);
  bool InitHidlServiceV2_0(android::sp<V2_0::ISensors> aServiceV2_0);
  bool InitSensorsList();
  void StartPollingThread();
  size_t PollHal();
  size_t PollFmq();
  SensorData CreateSensorData(const hidl_sensors::Event aEvent);

  android::sp<ISensorsWrapper> mSensors;
  hidl_sensors::SensorInfo mSensorInfoList[NUM_SENSOR_TYPE];
  base::Thread* mPollingThread;
  SensorDataCallback mSensorDataCallback;

  std::array<hidl_sensors::Event, MAX_EVENT_BUFFER_SIZE> mEventBuffer;
  typedef MessageQueue<hidl_sensors::Event, kSynchronizedReadWrite> EventMessageQueue;
  std::unique_ptr<EventMessageQueue> mEventQueue;
  typedef MessageQueue<uint32_t, kSynchronizedReadWrite> WakeLockQueue;
  std::unique_ptr<WakeLockQueue> mWakeLockQueue;
  EventFlag* mEventQueueFlag;

  const int64_t kDefaultSamplingPeriodNs = 200000000;
  const int64_t kPressureSamplingPeriodNs = 1000000000;
  const int64_t kReportLatencyNs = 0;
};

GonkSensorsHal* GonkSensorsHal::sInstance = nullptr;


} // hal_impl
} // mozilla

#endif // HAL_GONK_GONKSENSORSHAL_H_
