/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* Copyright 2012 Mozilla Foundation and Mozilla contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GonkGPSGeolocationProvider.h"

#include "GeolocationUtil.h"
#include <hardware_legacy/power.h>
#include "mozilla/dom/GeolocationPosition.h"
#include "mozilla/Preferences.h"
#include "mozilla/Services.h"
#include "nsComponentManagerUtils.h"
#include "nsIRunnable.h"
#include "nsThreadUtils.h"
#include "nsIThread.h"
#include "nsIURLFormatter.h"
#include "nsServiceManagerUtils.h"
#include "prtime.h"  // for PR_Now()

#ifdef MOZ_B2G_RIL
#  include "mozilla/SVGContentUtils.h"  // for ParseInteger
#  include "nsIDataCallManager.h"
#  include "nsIIccInfo.h"
#  include "nsIIccService.h"
#  include "nsIMobileCellInfo.h"
#  include "nsIMobileConnectionInfo.h"
#  include "nsIMobileConnectionService.h"
#  include "nsIMobileNetworkInfo.h"
#  include "nsINetworkInterface.h"  // for nsINetworkInfo
#  include "nsINetworkInterfaceListService.h"
#  include "nsINetworkManager.h"
#  include "nsIObserverService.h"
#  include "nsIRadioInterfaceLayer.h"
#  include "nsITelephonyCallInfo.h"
#  include "nsPrintfCString.h"
#endif

#undef LOG
#undef ERR
#undef DBG
#define LOG(args...) \
  __android_log_print(ANDROID_LOG_INFO, "GonkGPS_GEO", ##args)
#define ERR(args...) \
  __android_log_print(ANDROID_LOG_ERROR, "GonkGPS_GEO", ##args)

// TODO: Don't prrint DBG message when 'gDebug_isLoggingEnabled' is false
#define DBG(args...)                                                 \
  do {                                                               \
    if (true) {                                                      \
      __android_log_print(ANDROID_LOG_DEBUG, "GonkGPS_GEO", ##args); \
    }                                                                \
  } while (0)

using namespace mozilla;
using namespace mozilla::dom;

using android::hardware::Return;
using android::hardware::Void;

using IGnss_V1_0 = android::hardware::gnss::V1_0::IGnss;
using IGnssCallback_V1_0 = android::hardware::gnss::V1_0::IGnssCallback;
using GnssLocation_V1_0 = android::hardware::gnss::V1_0::GnssLocation;

using android::hardware::hidl_vec;
using android::hardware::gnss::V2_0::IGnssCallback;
using IGnss_V1_1 = android::hardware::gnss::V1_1::IGnss;
using IGnss_V2_0 = android::hardware::gnss::V2_0::IGnss;
using GnssLocation_V2_0 = android::hardware::gnss::V2_0::GnssLocation;

using IAGnssRil_V2_0 = android::hardware::gnss::V2_0::IAGnssRil;
using IAGnss_V2_0 = android::hardware::gnss::V2_0::IAGnss;
using IAGnssCallback_V2_0 = android::hardware::gnss::V2_0::IAGnssCallback;

using android::hardware::gnss::visibility_control::V1_0::IGnssVisibilityControl;
using android::hardware::gnss::visibility_control::V1_0::
    IGnssVisibilityControlCallback;

// Implements the callback methods for IGnssCallback interface.
struct GnssCallback : public IGnssCallback {
  Return<void> gnssLocationCb(const GnssLocation_V1_0& location) override;
  Return<void> gnssStatusCb(
      const IGnssCallback_V1_0::GnssStatusValue status) override;
  Return<void> gnssSvStatusCb(
      const IGnssCallback_V1_0::GnssSvStatus& svStatus) override;
  Return<void> gnssNmeaCb(int64_t timestamp,
                          const android::hardware::hidl_string& nmea) override;
  Return<void> gnssSetCapabilitesCb(uint32_t capabilities) override;
  Return<void> gnssAcquireWakelockCb() override;
  Return<void> gnssReleaseWakelockCb() override;
  Return<void> gnssRequestTimeCb() override;
  Return<void> gnssSetSystemInfoCb(
      const IGnssCallback::GnssSystemInfo& info) override;

  // New in gnss 1.1
  Return<void> gnssNameCb(const android::hardware::hidl_string& name) override;
  Return<void> gnssRequestLocationCb(const bool independentFromGnss) override;

  // New in gnss 2.0
  Return<void> gnssRequestLocationCb_2_0(const bool independentFromGnss,
                                         const bool isUserEmergency) override;
  Return<void> gnssSetCapabilitiesCb_2_0(uint32_t capabilities) override;
  Return<void> gnssLocationCb_2_0(const GnssLocation_V2_0& location) override;
  Return<void> gnssSvStatusCb_2_0(
      const hidl_vec<IGnssCallback::GnssSvInfo>& svInfoList) override;
};
static android::sp<IGnssCallback> gnssCbIface = nullptr;

#ifdef MOZ_B2G_RIL
// Implements the callback methods for IAGnssCallback_V2_0 interface.
struct AGnssCallback_V2_0 : public IAGnssCallback_V2_0 {
  // methonds from ::android::hardware::gps::V2_0::IAGnssCallback
  Return<void> agnssStatusCb(
      IAGnssCallback_V2_0::AGnssType type,
      IAGnssCallback_V2_0::AGnssStatusValue status) override;
};
static android::sp<IAGnssCallback_V2_0> agnssCbIface = nullptr;
#endif

// Implements the callback methods for IGnssVisibilityControl interface.
struct GnssVisibilityControlCallback : public IGnssVisibilityControlCallback {
  Return<void> nfwNotifyCb(
      const IGnssVisibilityControlCallback::NfwNotification& notification)
      override;
  Return<bool> isInEmergencySession() override;
};
static android::sp<IGnssVisibilityControlCallback> gnssVcCbIface = nullptr;

static const int kDefaultPeriod = 1000;  // ms

static bool gGeolocationEnabled = false;
static bool gDebug_isLoggingEnabled = false;
static bool gDebug_isGPSLocationIgnored = false;
static bool gIsInEmergencySession = false;

// Clean up GPS HAL when Geolocation setting is turned off.
static const char* kPrefOndemandCleanup = "geo.provider.ondemand_cleanup";

static const char* kWakeLockName = "GeckoGPS";

// The geolocation enabled setting
static const auto kSettingGeolocationEnabled = u"geolocation.enabled"_ns;

// Both of these settings can be toggled in the Gaia Developer settings screen.
static const auto kSettingDebugEnabled = u"geolocation.debugging.enabled"_ns;
static const auto kSettingDebugGpsIgnored =
    u"geolocation.debugging.gps-locations-ignored"_ns;

#ifdef MOZ_B2G_RIL
static const char* kNetworkActiveChangedTopic = "network-active-changed";
static const char* kNetworkConnStateChangedTopic =
    "network-connection-state-changed";
static const char* kPrefRilNumRadioInterfaces = "ril.numRadioInterfaces";
static const auto kSettingRilDefaultServiceId = u"ril.data.defaultServiceId"_ns;
#endif

NS_IMPL_ISUPPORTS(GonkGPSGeolocationProvider::NetworkLocationUpdate,
                  nsIGeolocationUpdate)

void GonkGPSGeolocationProvider::InitGnssHal() {
  mGnssHal_V2_0 = IGnss_V2_0::getService();
  if (mGnssHal_V2_0 != nullptr) {
    mGnssHal = mGnssHal_V2_0;
    mGnssHal_V1_1 = mGnssHal_V2_0;
#ifdef MOZ_B2G_RIL
    auto agnss_V2_0 = mGnssHal_V2_0->getExtensionAGnss_2_0();
    if (agnss_V2_0.isOk()) {
      mAGnssHal_V2_0 = agnss_V2_0;
    } else {
      ERR("Unable to get a handle to IAGnss_V2_0");
    }
    auto agnssRil_V2_0 = mGnssHal_V2_0->getExtensionAGnssRil_2_0();
    if (agnssRil_V2_0.isOk()) {
      mAGnssRilHal_V2_0 = agnssRil_V2_0;
    } else {
      ERR("Unable to get a handle to IAGnssRil_V2_0");
    }
#endif
    auto gnssVisibilityControl = mGnssHal_V2_0->getExtensionVisibilityControl();
    if (gnssVisibilityControl.isOk()) {
      mGnssVisibilityControlHal = gnssVisibilityControl;
    } else {
      ERR("Unable to get a handle to IGnssVisibilityControl");
    }
    return;
  }

  LOG("gnssHal 2.0 was null, trying 1.1");
  mGnssHal_V1_1 = IGnss_V1_1::getService();
  if (mGnssHal_V1_1 != nullptr) {
    mGnssHal = mGnssHal_V1_1;
    return;
  }

  LOG("gnssHal 1.1 was null, trying 1.0");
  mGnssHal = IGnss_V1_0::getService();
}

NS_IMETHODIMP
GonkGPSGeolocationProvider::NetworkLocationUpdate::Update(
    nsIDOMGeoPosition* position) {
  RefPtr<GonkGPSGeolocationProvider> provider =
      GonkGPSGeolocationProvider::GetSingleton();

  nsCOMPtr<nsIDOMGeoPositionCoords> coords;
  position->GetCoords(getter_AddRefs(coords));
  if (!coords) {
    return NS_ERROR_FAILURE;
  }

  double lat, lon, acc;
  coords->GetLatitude(&lat);
  coords->GetLongitude(&lon);
  coords->GetAccuracy(&acc);

  double delta = -1.0;

  static double sLastMLSPosLat = 0.0;
  static double sLastMLSPosLon = 0.0;

  if (0 != sLastMLSPosLon || 0 != sLastMLSPosLat) {
    delta = CalculateDeltaInMeter(lat, lon, sLastMLSPosLat, sLastMLSPosLon);
  }

  sLastMLSPosLat = lat;
  sLastMLSPosLon = lon;

  // if the MLS coord change is smaller than this arbitrarily small value
  // assume the MLS coord is unchanged, and stick with the GPS location
  const double kMinMLSCoordChangeInMeters = 10.0;

  DOMTimeStamp time_ms = 0;
  if (provider->mLastGPSPosition) {
    provider->mLastGPSPosition->GetTimestamp(&time_ms);
  }
  const int64_t diff_ms = (PR_Now() / PR_USEC_PER_MSEC) - time_ms;

  // We want to distinguish between the GPS being inactive completely
  // and temporarily inactive. In the former case, we would use a low
  // accuracy network location; in the latter, we only want a network
  // location that appears to updating with movement.

  const bool isGPSFullyInactive = diff_ms > 1000 * 60 * 2;  // two mins
  const bool isGPSTempInactive = diff_ms > 1000 * 10;       // 10 secs

  if (provider->mLocationCallback) {
    if (isGPSFullyInactive ||
        (isGPSTempInactive && delta > kMinMLSCoordChangeInMeters)) {
      DBG("Using MLS, GPS age:%fs, MLS Delta:%fm", diff_ms / 1000.0, delta);
      provider->mLocationCallback->Update(position);
    } else if (provider->mLastGPSPosition) {
      DBG("Using old GPS age:%fs", diff_ms / 1000.0);

      // This is a fallback case so that the GPS provider responds with its last
      // location rather than waiting for a more recent GPS or network location.
      // The service decides if the location is too old, not the provider.
      provider->mLocationCallback->Update(provider->mLastGPSPosition);
    }
  }

  provider->InjectLocation(lat, lon, acc);

  if (provider->mLocationCallback) {
    LOG("Got network position");
    provider->mLocationCallback->Update(position);
  }

  return NS_OK;
}

NS_IMETHODIMP
GonkGPSGeolocationProvider::NetworkLocationUpdate::NotifyError(uint16_t error) {
  return NS_OK;
}

// While most methods of GonkGPSGeolocationProvider should only be
// called from main thread, we deliberately put the Init and ShutdownGPS
// methods off main thread to avoid blocking.
NS_IMPL_ISUPPORTS(GonkGPSGeolocationProvider, nsIGeolocationProvider,
#ifdef MOZ_B2G_RIL
                  nsIObserver, nsITelephonyListener,
#endif
                  nsISettingsGetResponse, nsISettingsObserver,
                  nsISidlDefaultResponse)

// Static members
GonkGPSGeolocationProvider* GonkGPSGeolocationProvider::sSingleton = nullptr;
#ifdef MOZ_B2G_RIL
nsString GonkGPSGeolocationProvider::sSettingRilDataApn =
    u"ril.data.apn.sim1"_ns;
nsString GonkGPSGeolocationProvider::sSettingRilSuplApn =
    u"ril.supl.apn.sim1"_ns;
nsString GonkGPSGeolocationProvider::sSettingRilSuplEsApn =
    u"ril.emergency.apn.sim1"_ns;
nsString GonkGPSGeolocationProvider::sSettingRilSuplProtocol =
    u"ril.supl.protocol.sim1"_ns;
nsString GonkGPSGeolocationProvider::sSettingRilSuplEsProtocol =
    u"ril.emergency.protocol.sim1"_ns;
nsString GonkGPSGeolocationProvider::sSettingRilSuplRoamingProtocol =
    u"ril.supl.roaming_protocol.sim1"_ns;
nsString GonkGPSGeolocationProvider::sSettingRilSuplEsRoamingProtocol =
    u"ril.emergency.roaming_protocol.sim1"_ns;
nsCString GonkGPSGeolocationProvider::sRilDataApn;
nsCString GonkGPSGeolocationProvider::sRilSuplApn;
nsCString GonkGPSGeolocationProvider::sRilSuplEsApn;

// Static variables
static IAGnss_V2_0::ApnIpType sIpTypeSupl = IAGnss_V2_0::ApnIpType::IPV4V6;
static IAGnss_V2_0::ApnIpType sIpTypeSuplEs = IAGnss_V2_0::ApnIpType::IPV4V6;
static IAGnss_V2_0::ApnIpType sIpTypeSuplRoaming =
    IAGnss_V2_0::ApnIpType::IPV4V6;
static IAGnss_V2_0::ApnIpType sIpTypeSuplEsRoaming =
    IAGnss_V2_0::ApnIpType::IPV4V6;
#endif

GonkGPSGeolocationProvider::GonkGPSGeolocationProvider()
    : mGnssHalReady(false),
      mStarted(false),
      mSupportsScheduling(false),
      mSupportsSingleShot(false),
      mSupportsTimeInjection(false),
      mSupportsMSB(true),  // almost all the GNSS HALs support MSB
      mSupportsMSA(false),
#ifdef MOZ_B2G_RIL
      mRilDataServiceId(0),
      mNumberOfRilServices(1),
      mSuplNetId(0),    // 0 represents "network unspecified"
      mSuplEsNetId(0),  // 0 represents "network unspecified"
      mActiveNetId(0),  // 0 represents "network unspecified"
      mActiveType(nsINetworkInfo::NETWORK_TYPE_UNKNOWN),
      mActiveCapabilities(0),
#endif
      mEnableHighAccuracy(false) {
  MOZ_ASSERT(NS_IsMainThread());

  // Initialize GNSS HALs
  nsresult rv = NS_NewNamedThread("GPS Init", getter_AddRefs(mInitThread));
  if (NS_SUCCEEDED(rv)) {
    RefPtr<GonkGPSGeolocationProvider> self = this;
    nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
        "GonkGPSGeolocationProvider::GonkGPSGeolocationProvider",
        [self]() { self->Init(); });
    mInitThread->Dispatch(r, NS_DISPATCH_NORMAL);
  } else {
    ERR("Failed to create GPS Init thread!");
    mInitThread = nullptr;
  }

  nsCOMPtr<nsISettingsManager> settings =
      do_GetService("@mozilla.org/sidl-native/settings;1");
  if (settings) {
    settings->Get(kSettingGeolocationEnabled, this);
    settings->Get(kSettingDebugEnabled, this);
    settings->Get(kSettingDebugGpsIgnored, this);
    settings->AddObserver(kSettingGeolocationEnabled, this, this);
    settings->AddObserver(kSettingDebugEnabled, this, this);
    settings->AddObserver(kSettingDebugGpsIgnored, this, this);
#ifdef MOZ_B2G_RIL
    settings->Get(sSettingRilDataApn, this);
    settings->AddObserver(sSettingRilDataApn, this, this);
    settings->Get(sSettingRilSuplApn, this);
    settings->AddObserver(sSettingRilSuplApn, this, this);
    settings->Get(sSettingRilSuplEsApn, this);
    settings->AddObserver(sSettingRilSuplEsApn, this, this);
    settings->Get(sSettingRilSuplProtocol, this);
    settings->AddObserver(sSettingRilSuplProtocol, this, this);
    settings->Get(sSettingRilSuplEsProtocol, this);
    settings->AddObserver(sSettingRilSuplEsProtocol, this, this);
    settings->Get(sSettingRilSuplRoamingProtocol, this);
    settings->AddObserver(sSettingRilSuplRoamingProtocol, this, this);
    settings->Get(sSettingRilSuplEsRoamingProtocol, this);
    settings->AddObserver(sSettingRilSuplEsRoamingProtocol, this, this);
#endif  // MOZ_B2G_RIL
  }

#ifdef MOZ_B2G_RIL
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs ||
      NS_FAILED(obs->AddObserver(this, kNetworkActiveChangedTopic, false))) {
    ERR("Failed to add active network changed observer!");
  }
  if (!obs ||
      NS_FAILED(obs->AddObserver(this, kNetworkConnStateChangedTopic, false))) {
    ERR("Failed to add network connection state changed observer!");
  }
#endif  // MOZ_B2G_RIL

  // Listen TelephonyService for updating gIsInEmergencySession
  ListenTelephonyService(true);
}

GonkGPSGeolocationProvider::~GonkGPSGeolocationProvider() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mStarted, "Must call Shutdown before destruction");

  if (mGnssHal != nullptr) {
    DBG("mGnssHal->cleanup()");
    mGnssHal->cleanup();
  }

  sSingleton = nullptr;

  nsCOMPtr<nsISettingsManager> settings =
      do_GetService("@mozilla.org/sidl-native/settings;1");
  if (settings) {
    settings->RemoveObserver(kSettingGeolocationEnabled, this, this);
    settings->RemoveObserver(kSettingDebugEnabled, this, this);
    settings->RemoveObserver(kSettingDebugGpsIgnored, this, this);
#ifdef MOZ_B2G_RIL
    settings->RemoveObserver(sSettingRilDataApn, this, this);
    settings->RemoveObserver(sSettingRilSuplApn, this, this);
    settings->RemoveObserver(sSettingRilSuplEsApn, this, this);
    settings->RemoveObserver(sSettingRilSuplProtocol, this, this);
    settings->RemoveObserver(sSettingRilSuplEsProtocol, this, this);
    settings->RemoveObserver(sSettingRilSuplRoamingProtocol, this, this);
    settings->RemoveObserver(sSettingRilSuplEsRoamingProtocol, this, this);
#endif  // MOZ_B2G_RIL
  }

#ifdef MOZ_B2G_RIL
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs ||
      NS_FAILED(obs->RemoveObserver(this, kNetworkActiveChangedTopic))) {
    ERR("Failed to remove active network changed observer!");
  }
  if (!obs ||
      NS_FAILED(obs->RemoveObserver(this, kNetworkConnStateChangedTopic))) {
    ERR("Failed to remove network connection state changed observer!");
  }
#endif  // MOZ_B2G_RIL

  // Stop listen TelephonyService
  ListenTelephonyService(false);
}

already_AddRefed<GonkGPSGeolocationProvider>
GonkGPSGeolocationProvider::GetSingleton() {
  MOZ_ASSERT(NS_IsMainThread());

  if (!sSingleton) sSingleton = new GonkGPSGeolocationProvider();

  RefPtr<GonkGPSGeolocationProvider> provider = sSingleton;
  return provider.forget();
}

NS_IMETHODIMP
GonkGPSGeolocationProvider::Startup() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mStarted) {
    return NS_OK;
  }

  // Setup NetworkLocationProvider if the API key and server URI are available
  nsAutoString serverUri;
  nsresult rv = Preferences::GetString("geo.wifi.uri", serverUri);
  if (NS_SUCCEEDED(rv) && !serverUri.IsEmpty()) {
    // nsresult rv;
    nsCOMPtr<nsIURLFormatter> formatter =
        do_CreateInstance("@mozilla.org/toolkit/URLFormatterService;1", &rv);
    if (NS_SUCCEEDED(rv)) {
      nsString key;
      rv = formatter->FormatURLPref(u"geo.authorization.key"_ns, key);
      if (NS_SUCCEEDED(rv) && !key.IsEmpty()) {
        mNetworkLocationProvider = do_CreateInstance(
            "@mozilla.org/geolocation/gonk-network-geolocation-provider;1");
        if (mNetworkLocationProvider) {
          rv = mNetworkLocationProvider->Startup();
          if (NS_SUCCEEDED(rv)) {
            RefPtr<NetworkLocationUpdate> update = new NetworkLocationUpdate();
          }
        }
      }
    }
  }

  if (mGnssHalReady) {
    RefPtr<GonkGPSGeolocationProvider> self = this;
    nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
        "GonkGPSGeolocationProvider::Startup", [self]() { self->StartGPS(); });
    NS_DispatchToMainThread(r);
  } else {
    ERR("Startup without initialization.");
  }

  mStarted = true;
  return NS_OK;
}

NS_IMETHODIMP
GonkGPSGeolocationProvider::Shutdown() {
  MOZ_ASSERT(NS_IsMainThread());

  if (!mStarted) {
    return NS_OK;
  }

  mStarted = false;

  if (mNetworkLocationProvider) {
    mNetworkLocationProvider->Shutdown();
    mNetworkLocationProvider = nullptr;
  }

  RefPtr<GonkGPSGeolocationProvider> self = this;
  nsCOMPtr<nsIRunnable> r =
      NS_NewRunnableFunction("GonkGPSGeolocationProvider::Shutdown",
                             [self]() { self->ShutdownGPS(); });
  mInitThread->Dispatch(r, NS_DISPATCH_NORMAL);

  return NS_OK;
}

NS_IMETHODIMP
GonkGPSGeolocationProvider::Watch(nsIGeolocationUpdate* aCallback) {
  MOZ_ASSERT(NS_IsMainThread());

  mLocationCallback = aCallback;
  return NS_OK;
}

NS_IMETHODIMP
GonkGPSGeolocationProvider::SetHighAccuracy(bool enableHighAccuracy) {
  if (mEnableHighAccuracy != enableHighAccuracy) {
    SetPositionMode(enableHighAccuracy);
  }

  mEnableHighAccuracy = enableHighAccuracy;
  return NS_OK;
}

void GonkGPSGeolocationProvider::GnssDeathRecipient::serviceDied(
    uint64_t cookie, const android::wp<android::hidl::base::V1_0::IBase>& who) {
  ERR("Abort due to IGNSS hidl service failure.");
}

void GonkGPSGeolocationProvider::Init() {
  // Must not be main thread. Some GPS driver's first init takes very long.
  MOZ_ASSERT(!NS_IsMainThread());

  InitGnssHal();
  if (mGnssHal == nullptr) {
    ERR("Failed to get Gnss HAL.");
    return;
  }

  mGnssHalDeathRecipient = new GnssDeathRecipient();
  Return<bool> linked =
      mGnssHal->linkToDeath(mGnssHalDeathRecipient, /*cookie*/ 0);
  if (!linked.isOk()) {
    ERR("Transaction error in linking to GnssHAL death: %s",
        linked.description().c_str());
  } else if (!linked) {
    ERR("Unable to link to GnssHal death notifications");
  } else {
    DBG("Link to death notification successful");
  }

#ifdef MOZ_B2G_RIL
  if (mAGnssHal_V2_0 != nullptr) {
    if (!agnssCbIface) {
      agnssCbIface = new AGnssCallback_V2_0();
    }
    DBG("mAGnssHal_V2_0->setCallback");
    Return<void> agnssStatus = mAGnssHal_V2_0->setCallback(agnssCbIface);
    if (!agnssStatus.isOk()) {
      ERR("failed to set callback for IAGnss");
    }
  }

  // report network state to IAGnssRil during the initialization since the
  // information may be needed by HAL for network positioning integration
  RefPtr<GonkGPSGeolocationProvider> self = this;
  nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
      "GonkGPSGeolocationProvider::UpdateNetworkState",
      [self]() { self->UpdateNetworkState(nullptr, false); });
  NS_DispatchToMainThread(r);
#endif

  if (mGnssVisibilityControlHal != nullptr) {
    if (!gnssVcCbIface) {
      gnssVcCbIface = new GnssVisibilityControlCallback();
    }
    Return<bool> gnssVcResult =
        mGnssVisibilityControlHal->setCallback(gnssVcCbIface);
    if (!gnssVcResult.isOk() || !gnssVcResult) {
      ERR("SetCallback for Gnss VC Interface fails");
    }
  }

#ifdef MOZ_B2G_RIL
  r = NS_NewRunnableFunction("GonkGPSGeolocationProvider::Init",
                             [self]() { self->SetupAGPS(); });
  NS_DispatchToMainThread(r);
#endif

  LOG("GNSS HAL has been initialized");
}

void GonkGPSGeolocationProvider::SetupGnssHal() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mGnssHal == nullptr) {
    ERR("Gnss HAL hasn't initialized");
    return;
  }

  if (!gnssCbIface) {
    gnssCbIface = new GnssCallback();
  }

  Return<bool> result = false;
  if (mGnssHal_V2_0 != nullptr) {
    DBG("mGnssHal_V2_0->setCallback_2_0");
    result = mGnssHal_V2_0->setCallback_2_0(gnssCbIface);
  } else if (mGnssHal_V1_1 != nullptr) {
    DBG("mGnssHal_V1_1->setCallback_1_1");
    result = mGnssHal_V1_1->setCallback_1_1(gnssCbIface);
  } else {
    DBG("mGnssHal->setCallback");
    result = mGnssHal->setCallback(gnssCbIface);
  }

  if (!result.isOk() || !result) {
    ERR("SetCallback for Gnss Interface fails");
  }

  mGnssHalReady = true;

  LOG("GNSS HAL is ready for location callbacks");

  // If there is an ongoing location request, starts GPS navigating
  if (mStarted) {
    StartGPS();
  }
}

void GonkGPSGeolocationProvider::CleanupGnssHal() {
  DBG("mGnssHal->stop and cleanup");
  // Cleanup GNSS HAL when Geolocation setting is turned off
  if (mGnssHal != nullptr) {
    auto result = mGnssHal->stop();
    if (!result.isOk() || !result) {
      ERR("failed to stop IGnss HAL");
    }

    mGnssHal->cleanup();
  }
  mGnssHalReady = false;
}

void GonkGPSGeolocationProvider::StartGPS() {
  MOZ_ASSERT(NS_IsMainThread());
  LOG("Starts GPS");

#ifdef MOZ_B2G_RIL
  SetupAGPS();
#endif

  MOZ_ASSERT(mGnssHal != nullptr);

  bool success = SetPositionMode(mEnableHighAccuracy);
  if (!success) {
    return;
  }

  DBG("mGnssHal->start");
  Return<bool> result = mGnssHal->start();
  if (!result.isOk() || !result) {
    ERR("failed to start IGnss HAL");
  }
}

void GonkGPSGeolocationProvider::ShutdownGPS() {
  MOZ_ASSERT(!mStarted, "Should only be called after Shutdown");

  LOG("Stops GPS");

  if (mGnssHal != nullptr) {
    DBG("mGnssHal->stop");
    auto result = mGnssHal->stop();
    if (!result.isOk() || !result) {
      ERR("failed to stop IGnss HAL");
    }
  }
}

void GonkGPSGeolocationProvider::InjectLocation(double latitude,
                                                double longitude,
                                                float accuracy) {
  MOZ_ASSERT(NS_IsMainThread());

  DBG("injecting location (%f, %f) accuracy: %f", latitude, longitude,
      accuracy);

  if (mGnssHal != nullptr) {
    DBG("mGnssHal->injectLocation");
    auto result = mGnssHal->injectLocation(latitude, longitude, accuracy);
    if (!result.isOk() || !result) {
      ERR("%s: Gnss injectLocation() failed");
    }
  }
}

bool GonkGPSGeolocationProvider::SetPositionMode(bool enableHighAccuracy) {
  if (mGnssHal == nullptr) {
    return false;
  }

  int32_t update = Preferences::GetInt("geo.default.update", kDefaultPeriod);
  if (!mSupportsScheduling) {
    update = kDefaultPeriod;
  }

  int preferredAccuracy = enableHighAccuracy ? 0 : 10000;  // 10km
  DBG("SetPositionMode: update: %d, preferredAccuracy: %d", update,
      preferredAccuracy);

  Return<bool> result = false;
  if (mGnssHal_V1_1 != nullptr) {
    auto positionMode = mSupportsMSB ? IGnss_V1_1::GnssPositionMode::MS_BASED
                                     : IGnss_V1_1::GnssPositionMode::STANDALONE;
    result = mGnssHal_V1_1->setPositionMode_1_1(
        positionMode, IGnss_V1_1::GnssPositionRecurrence::RECURRENCE_PERIODIC,
        update, preferredAccuracy,
        0,       // preferred time
        false);  // low power mode
  } else if (mGnssHal != nullptr) {
    auto positionMode = mSupportsMSB ? IGnss_V1_1::GnssPositionMode::MS_BASED
                                     : IGnss_V1_1::GnssPositionMode::STANDALONE;
    result = mGnssHal->setPositionMode(
        positionMode, IGnss_V1_0::GnssPositionRecurrence::RECURRENCE_PERIODIC,
        update, preferredAccuracy,
        0);  // preferred time
  }

  if (!result.isOk() || !result) {
    ERR("failed to set IGnss position mode");
    return false;
  }

  return true;
}

class GonkGPSGeolocationProvider::UpdateLocationEvent final
    : public mozilla::Runnable {
 public:
  explicit UpdateLocationEvent(nsGeoPosition* aPosition)
      : mozilla::Runnable("UpdateLocationEvent"), mPosition(aPosition) {}
  NS_IMETHOD Run() {
    RefPtr<GonkGPSGeolocationProvider> provider =
        GonkGPSGeolocationProvider::GetSingleton();
    nsCOMPtr<nsIGeolocationUpdate> callback = provider->mLocationCallback;
    provider->mLastGPSPosition = mPosition;
    if (callback) {
      callback->Update(mPosition);
    }
    return NS_OK;
  }

 private:
  RefPtr<nsGeoPosition> mPosition;
};

class GonkGPSGeolocationProvider::UpdateCapabilitiesEvent final
    : public mozilla::Runnable {
 public:
  explicit UpdateCapabilitiesEvent(uint32_t aCapabilities)
      : mozilla::Runnable("UpdateCapabilitiesEvent"),
        mCapabilities(aCapabilities) {}
  NS_IMETHOD Run() {
    RefPtr<GonkGPSGeolocationProvider> provider =
        GonkGPSGeolocationProvider::GetSingleton();

    provider->mSupportsScheduling =
        mCapabilities & IGnssCallback::Capabilities::SCHEDULING;
    provider->mSupportsMSB = mCapabilities & IGnssCallback::Capabilities::MSB;
    provider->mSupportsMSA = mCapabilities & IGnssCallback::Capabilities::MSA;
    provider->mSupportsSingleShot =
        mCapabilities & IGnssCallback::Capabilities::SINGLE_SHOT;
    provider->mSupportsTimeInjection =
        mCapabilities & IGnssCallback::Capabilities::ON_DEMAND_TIME;

    return NS_OK;
  }

 private:
  uint32_t mCapabilities;
};

Return<void> GnssCallback::gnssLocationCb(const GnssLocation_V1_0& location) {
  if (gDebug_isGPSLocationIgnored) {
    LOG("gnssLocationCb is ignored due to the developer setting");
    return Void();
  }

  const float kImpossibleAccuracy_m = 0.001;
  if (location.horizontalAccuracyMeters < kImpossibleAccuracy_m) {
    ERR("%s: horizontalAccuracyMeters < kImpossibleAccuracy_m", __FUNCTION__);
    return Void();
  }

  RefPtr<nsGeoPosition> somewhere = new nsGeoPosition(
      location.latitudeDegrees, location.longitudeDegrees,
      location.altitudeMeters, location.horizontalAccuracyMeters,
      location.verticalAccuracyMeters, location.bearingDegrees,
      location.speedMetersPerSec, PR_Now() / PR_USEC_PER_MSEC);
  // Note above: Can't use location->timestamp as the time from the satellite is
  // a minimum of 16 secs old (see http://leapsecond.com/java/gpsclock.htm). All
  // code from this point on expects the gps location to be timestamped with the
  // current time, most notably: the geolocation service which respects
  // maximumAge set in the DOM JS.

  DBG("geo: GPS got a fix (%f, %f). accuracy: %f", location.latitudeDegrees,
      location.longitudeDegrees, location.horizontalAccuracyMeters);

  nsCOMPtr<nsIRunnable> runnable =
      new GonkGPSGeolocationProvider::UpdateLocationEvent(somewhere);
  NS_DispatchToMainThread(runnable);

  return Void();
}

Return<void> GnssCallback::gnssStatusCb(
    const IGnssCallback_V1_0::GnssStatusValue status) {
  const char* msgStream = 0;
  switch (status) {
    case IGnssCallback::GnssStatusValue::NONE:
      msgStream = "geo: GnssStatusValue::NONE\n";
      break;
    case IGnssCallback::GnssStatusValue::SESSION_BEGIN:
      msgStream = "geo: GnssStatusValue::SESSION_BEGIN\n";
      break;
    case IGnssCallback::GnssStatusValue::SESSION_END:
      msgStream = "geo: GnssStatusValue::SESSION_END\n";
      break;
    case IGnssCallback::GnssStatusValue::ENGINE_ON:
      msgStream = "geo: GnssStatusValue::ENGINE_ON\n";
      break;
    case IGnssCallback::GnssStatusValue::ENGINE_OFF:
      msgStream = "geo: GnssStatusValue::ENGINE_OFF\n";
      break;
    default:
      msgStream = "geo: Unknown GNSS status\n";
      break;
  }
  DBG("%s", msgStream);
  return Void();
}

Return<void> GnssCallback::gnssSvStatusCb(
    const IGnssCallback_V1_0::GnssSvStatus& svStatus) {
  DBG("%s: numSvs: %u", __FUNCTION__, svStatus.numSvs);
  return Void();
}

Return<void> GnssCallback::gnssNmeaCb(
    int64_t timestamp, const ::android::hardware::hidl_string& nmea) {
  DBG("%s: timestamp: %lld", __FUNCTION__, timestamp);
  return Void();
}

Return<void> GnssCallback::gnssSetCapabilitesCb(uint32_t capabilities) {
  DBG("%s: capabilities: %u", __FUNCTION__, capabilities);
  NS_DispatchToMainThread(
      new GonkGPSGeolocationProvider::UpdateCapabilitiesEvent(capabilities));
  return Void();
}

Return<void> GnssCallback::gnssAcquireWakelockCb() {
  DBG("%s", __FUNCTION__);
  acquire_wake_lock(PARTIAL_WAKE_LOCK, kWakeLockName);
  return Void();
}

Return<void> GnssCallback::gnssReleaseWakelockCb() {
  DBG("%s", __FUNCTION__);
  release_wake_lock(kWakeLockName);
  return Void();
}

Return<void> GnssCallback::gnssRequestTimeCb() {
  LOG("%s: Gonk doesn't support time injection.", __FUNCTION__);
  return Void();
}

Return<void> GnssCallback::gnssSetSystemInfoCb(
    const IGnssCallback::GnssSystemInfo& info) {
  DBG("%s: yearOfHw: %u", __FUNCTION__, info.yearOfHw);
  return Void();
}

Return<void> GnssCallback::gnssNameCb(
    const android::hardware::hidl_string& name) {
  LOG("%s: name=%s", __FUNCTION__, name.c_str());
  return Void();
}

Return<void> GnssCallback::gnssRequestLocationCb(
    const bool independentFromGnss) {
  DBG("%s: Gonk doesn't support gnssRequestLocationCb.", __FUNCTION__);
  return Void();
}

Return<void> GnssCallback::gnssRequestLocationCb_2_0(
    const bool independentFromGnss, const bool isUserEmergency) {
  DBG("%s: Gonk doesn't support gnssRequestLocationCb_2_0.", __FUNCTION__);
  return Void();
}

Return<void> GnssCallback::gnssSetCapabilitiesCb_2_0(uint32_t capabilities) {
  return gnssSetCapabilitesCb(capabilities);
}

Return<void> GnssCallback::gnssLocationCb_2_0(
    const GnssLocation_V2_0& location) {
  return gnssLocationCb(location.v1_0);
}

Return<void> GnssCallback::gnssSvStatusCb_2_0(
    const hidl_vec<IGnssCallback::GnssSvInfo>& svInfoList) {
  DBG("%s: numSvs: %u", __FUNCTION__, svInfoList.size());
  return Void();
}

Return<void> GnssVisibilityControlCallback::nfwNotifyCb(
    const IGnssVisibilityControlCallback::NfwNotification& notification) {
  DBG("%s: Gonk doesn't handle NfwNotification.", __FUNCTION__);
  return Void();
}

Return<bool> GnssVisibilityControlCallback::isInEmergencySession() {
  DBG("%s: gIsInEmergencySession is %d", __FUNCTION__, gIsInEmergencySession);
  return Return<bool>(gIsInEmergencySession);
}

NS_IMETHODIMP GonkGPSGeolocationProvider::HandleSettings(
    nsISettingInfo* const info, [[maybe_unused]] bool isObserved) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!info) {
    return NS_OK;
  }
  nsString name;
  info->GetName(name);

  nsString value;
  info->GetValue(value);

  if (name.Equals(kSettingDebugGpsIgnored)) {
    gDebug_isGPSLocationIgnored = value.EqualsLiteral("true");
    LOG("ObserveSetting: ignoring: %d", gDebug_isGPSLocationIgnored);
  } else if (name.Equals(kSettingDebugEnabled)) {
    gDebug_isLoggingEnabled = value.EqualsLiteral("true");
    LOG("ObserveSetting: logging: %d", gDebug_isLoggingEnabled);
  } else if (name.Equals(kSettingGeolocationEnabled)) {
    gGeolocationEnabled = value.EqualsLiteral("true");
    LOG("ObserveSetting: geolocation-enabled: %d", gGeolocationEnabled);

    // Initialize GonkGPSGeolocationProvider
    if (gGeolocationEnabled && !mGnssHalReady) {
      RefPtr<GonkGPSGeolocationProvider> self = this;
      nsCOMPtr<nsIRunnable> r =
          NS_NewRunnableFunction("GonkGPSGeolocationProvider::HandleSettings",
                                 [self]() { self->SetupGnssHal(); });
      NS_DispatchToMainThread(r);
    }

    // On demand cleanup
    if (!gGeolocationEnabled && mGnssHalReady &&
        Preferences::GetBool(kPrefOndemandCleanup)) {
      CleanupGnssHal();
    }

    // Handle NFW location access
    if (mGnssVisibilityControlHal) {
      hidl_vec<android::hardware::hidl_string> hidlProxyApps;
      if (gGeolocationEnabled) {
        hidlProxyApps = {"b2g_system"};
      }

      auto result =
          mGnssVisibilityControlHal->enableNfwLocationAccess(hidlProxyApps);
      if (!result.isOk() || !result) {
        ERR("Failed to enableNfwLocationAccess");
      }
    }
  }
#ifdef MOZ_B2G_RIL
  else if (name.Equals(sSettingRilDataApn)) {
    // Remove the surrounding " " of setting string
    value.Trim("\"");
    sRilDataApn = NS_ConvertUTF16toUTF8(value);
    DBG("ObserveSetting: data APN: %s", sRilDataApn.get());
  } else if (name.Equals(sSettingRilSuplApn)) {
    // Remove the surrounding " " of setting string
    value.Trim("\"");
    sRilSuplApn = NS_ConvertUTF16toUTF8(value);
    DBG("ObserveSetting: supl APN: %s", sRilSuplApn.get());
  } else if (name.Equals(sSettingRilSuplEsApn)) {
    // Remove the surrounding " " of setting string
    value.Trim("\"");
    sRilSuplEsApn = NS_ConvertUTF16toUTF8(value);
    DBG("ObserveSetting: supl es APN: %s", sRilSuplEsApn.get());
  } else if (name.Equals(kSettingRilDefaultServiceId)) {
    int32_t serviceId = 0;
    if (SVGContentUtils::ParseInteger(value, serviceId) == false) {
      ERR("'ril.data.defaultServiceId' is not a number!");
      return NS_ERROR_UNEXPECTED;
    }

    if (!IsValidRilServiceId(serviceId)) {
      ERR("serviceId is invalid");
      return NS_ERROR_UNEXPECTED;
    }

    // Update APN setting keys and observers when service id is changed
    if (mRilDataServiceId != static_cast<uint32_t>(serviceId)) {
      UpdateApnObservers(serviceId);
    }

    mRilDataServiceId = serviceId;
    UpdateRadioInterface();
  } else if (name.Equals(sSettingRilSuplProtocol)) {
    sIpTypeSupl = GetApnIpType(value);
    DBG("ObserveSetting: supl protocol: %hhu", sIpTypeSupl);
  } else if (name.Equals(sSettingRilSuplEsProtocol)) {
    sIpTypeSuplEs = GetApnIpType(value);
    DBG("ObserveSetting: supl es protocol: %hhu", sIpTypeSuplEs);
  } else if (name.Equals(sSettingRilSuplRoamingProtocol)) {
    sIpTypeSuplRoaming = GetApnIpType(value);
    DBG("ObserveSetting: supl roaming protocol: %hhu", sIpTypeSuplRoaming);
  } else if (name.Equals(sSettingRilSuplEsRoamingProtocol)) {
    sIpTypeSuplEsRoaming = GetApnIpType(value);
    DBG("ObserveSetting: supl es roaming protocol: %hhu", sIpTypeSuplEsRoaming);
  }
#endif  // MOZ_B2G_RIL
  return NS_OK;
}

// Implements nsISettingsGetResponse::Resolve
NS_IMETHODIMP GonkGPSGeolocationProvider::Resolve(nsISettingInfo* info) {
  return HandleSettings(info, false);
}

// Implements nsISettingsGetResponse::Reject
NS_IMETHODIMP GonkGPSGeolocationProvider::Reject(nsISettingError* error) {
  if (error) {
    nsString name;
    error->GetName(name);
    LOG("Failed to read %s", NS_ConvertUTF16toUTF8(name).get());
  }
  return NS_OK;
}

// Implements nsISettingsObserver::ObserveSetting
NS_IMETHODIMP GonkGPSGeolocationProvider::ObserveSetting(nsISettingInfo* info) {
  return HandleSettings(info, true);
}

// Implements nsISidlDefaultResponse
NS_IMETHODIMP GonkGPSGeolocationProvider::Resolve() { return NS_OK; }
NS_IMETHODIMP GonkGPSGeolocationProvider::Reject() { return NS_OK; }

#ifdef MOZ_B2G_RIL
Return<void> AGnssCallback_V2_0::agnssStatusCb(
    IAGnssCallback_V2_0::AGnssType type,
    IAGnssCallback_V2_0::AGnssStatusValue status) {
  class AGPSStatusEvent : public Runnable {
   public:
    AGPSStatusEvent(IAGnssCallback_V2_0::AGnssType aType,
                    IAGnssCallback_V2_0::AGnssStatusValue aStatus)
        : Runnable("AGPSStatusEvent"), mType(aType), mStatus(aStatus) {}
    NS_IMETHOD Run() override {
      RefPtr<GonkGPSGeolocationProvider> provider =
          GonkGPSGeolocationProvider::GetSingleton();

      switch (mStatus) {
        case IAGnssCallback_V2_0::AGnssStatusValue::REQUEST_AGNSS_DATA_CONN:
          DBG("agnssStatusCb, REQUEST_AGNSS_DATA_CONN, type: %d",
              static_cast<int>(mType));
          if (mType == IAGnssCallback_V2_0::AGnssType::SUPL) {
            provider->RequestDataConnection(false);
          } else if (mType == IAGnssCallback_V2_0::AGnssType::SUPL_EIMS) {
            // Request emergency data call if SUPL-ES APN is available
            provider->RequestDataConnection(
                !GonkGPSGeolocationProvider::sRilSuplEsApn.IsEmpty());
          }
          break;
        case IAGnssCallback_V2_0::AGnssStatusValue::RELEASE_AGNSS_DATA_CONN:
          DBG("agnssStatusCb, RELEASE_AGNSS_DATA_CONN, type: %d",
              static_cast<int>(mType));
          if (mType == IAGnssCallback_V2_0::AGnssType::SUPL) {
            provider->ReleaseDataConnection(false);
          } else if (mType == IAGnssCallback_V2_0::AGnssType::SUPL_EIMS) {
            // Release emergency data call if SUPL-ES APN is available
            provider->ReleaseDataConnection(
                !GonkGPSGeolocationProvider::sRilSuplEsApn.IsEmpty());
          }
          break;
        case IAGnssCallback_V2_0::AGnssStatusValue::AGNSS_DATA_CONNECTED:
          DBG("agnssStatusCb, AGNSS_DATA_CONNECTED");
          break;
        case IAGnssCallback_V2_0::AGnssStatusValue::AGNSS_DATA_CONN_DONE:
          DBG("agnssStatusCb, AGNSS_DATA_CONN_DONE");
          break;
        case IAGnssCallback_V2_0::AGnssStatusValue::AGNSS_DATA_CONN_FAILED:
          DBG("agnssStatusCb, AGNSS_DATA_CONN_FAILED");
          break;
      }
      return NS_OK;
    }

   private:
    IAGnssCallback_V2_0::AGnssType mType;
    IAGnssCallback_V2_0::AGnssStatusValue mStatus;
  };

  NS_DispatchToMainThread(new AGPSStatusEvent(type, status));

  return Void();
}

bool IsMetered(int aNetworkInterfaceType) {
  switch (aNetworkInterfaceType) {
    case nsINetworkInfo::NETWORK_TYPE_WIFI:
    case nsINetworkInfo::NETWORK_TYPE_MOBILE_ECC:
      return false;
    case nsINetworkInfo::NETWORK_TYPE_MOBILE:
    case nsINetworkInfo::NETWORK_TYPE_MOBILE_MMS:
    case nsINetworkInfo::NETWORK_TYPE_MOBILE_SUPL:
    case nsINetworkInfo::NETWORK_TYPE_MOBILE_DUN:
      return true;
    default:
      ERR(nsPrintfCString("Unknown network type mapping %d",
                          aNetworkInterfaceType)
              .get());
      return true;
  }
}

// Convert net id to net_handle_t and cast to uint64_t
static inline uint64_t GetNetHandle(int32_t aNetId) {
  if (!aNetId) {
    return 0;  // network unspecified
  }

  // The magic value of net handle which should correspond with the value in
  // system/netd/server/NetworkController.h
  constexpr uint32_t kHandleMagic = 0xcafed00d;
  return ((uint64_t)aNetId << 32) | kHandleMagic;
}

void GonkGPSGeolocationProvider::UpdateNetworkState(nsISupports* aNetworkInfo,
                                                    bool aObserved) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mAGnssRilHal_V2_0) {
    ERR("mAGnssRilHal_V2_0 isn't available for updating network state.");
    return;
  }

  nsCOMPtr<nsINetworkInfo> info;
  if (aNetworkInfo) {
    info = do_QueryInterface(aNetworkInfo);
  } else if (!aObserved) {
    nsCOMPtr<nsINetworkManager> networkManager =
        do_GetService("@mozilla.org/network/manager;1");
    if (!networkManager) {
      ERR("network manager isn't available");
      return;
    }
    networkManager->GetActiveNetworkInfo(getter_AddRefs(info));
  }

  int32_t type = nsINetworkInfo::NETWORK_TYPE_UNKNOWN;
  int32_t netId = 0;
  bool connected = false;
  uint16_t capabilities = 0;

  if (info) {
    int32_t state;
    info->GetState(&state);
    connected = (state == nsINetworkInfo::NETWORK_STATE_CONNECTED);

    info->GetNetId(&netId);
    info->GetType(&type);

    bool metered = IsMetered(type);
    bool roaming = false;
    nsCOMPtr<nsIRilNetworkInfo> rilInfo = do_QueryInterface(info);
    if (rilInfo) {
      roaming = IsRoaming();
    }

    if (!metered) {
      capabilities +=
          static_cast<uint16_t>(IAGnssRil_V2_0::NetworkCapability::NOT_METERED);
    }
    if (!roaming) {
      capabilities +=
          static_cast<uint16_t>(IAGnssRil_V2_0::NetworkCapability::NOT_ROAMING);
    }

    // "network-active-changed" topic wouldn't provide nsINetworkInfo when
    // it's disconnected, therefore, save netId / type / capabilities here.
    if (connected) {
      mActiveNetId = netId;
      mActiveType = type;
      mActiveCapabilities = capabilities;
    }
  } else {
    // These is no active network, it means the network has just disconnected
    netId = mActiveNetId;
    type = mActiveType;
    capabilities = mActiveCapabilities;
    mActiveNetId = 0;
    mActiveType = nsINetworkInfo::NETWORK_TYPE_UNKNOWN;
    mActiveCapabilities = 0;
  }

  uint64_t netHandle = GetNetHandle(netId);

  // Type of active network could be MOBILE, WiFi or ETHERNET, apn is only
  // needed when the type is MOBILE.
  auto apn = type == nsINetworkInfo::NETWORK_TYPE_MOBILE ? sRilDataApn.get()
                                                         : EmptyCString().get();

  IAGnssRil_V2_0::NetworkAttributes networkAttributes = {
      .networkHandle = netHandle,
      .isConnected = static_cast<bool>(connected),
      .capabilities = capabilities,
      .apn = apn,

  };
  DBG("updateNetworkState_2_0, netId: %d, netHandle: %llu, connected: %d, "
      "capabilities: %u, apn: %s)",
      netId, netHandle, connected, capabilities, apn);
  auto result = mAGnssRilHal_V2_0->updateNetworkState_2_0(networkAttributes);
  if (!result.isOk() || !result) {
    ERR("failed to update network state to IAGnssRil");
  }
}

NS_IMETHODIMP
GonkGPSGeolocationProvider::Observe(nsISupports* aSubject, const char* aTopic,
                                    const char16_t* aData) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!strcmp(aTopic, kNetworkActiveChangedTopic)) {
    UpdateNetworkState(aSubject, true);
  } else if (!strcmp(aTopic, kNetworkConnStateChangedTopic)) {
    HandleAGpsDataConnection(aSubject);
  }

  return NS_OK;
}

void GonkGPSGeolocationProvider::UpdateApnObservers(uint32_t aServiceId) {
  if (aServiceId >= 2) {
    ERR("failed to update APN observers, only SIM1 and SIM2 are supported.");
    return;
  }

  nsCOMPtr<nsISettingsManager> settings =
      do_GetService("@mozilla.org/sidl-native/settings;1");
  if (!settings) {
    ERR("failed to update APN observers, settings manager is unavailable.");
    return;
  }

  settings->RemoveObserver(sSettingRilDataApn, this, this);
  settings->RemoveObserver(sSettingRilSuplApn, this, this);
  settings->RemoveObserver(sSettingRilSuplEsApn, this, this);
  sSettingRilDataApn =
      aServiceId == 0 ? u"ril.data.apn.sim1"_ns : u"ril.data.apn.sim2"_ns;
  sSettingRilSuplApn =
      aServiceId == 0 ? u"ril.supl.apn.sim1"_ns : u"ril.supl.apn.sim2"_ns;
  sSettingRilSuplEsApn = aServiceId == 0 ? u"ril.emergency.apn.sim1"_ns
                                         : u"ril.emergency.apn.sim2"_ns;
  sSettingRilSuplProtocol = aServiceId == 0 ? u"ril.supl.protocol.sim1"_ns
                                            : u"ril.supl.protocol.sim2"_ns;
  sSettingRilSuplEsProtocol = aServiceId == 0
                                  ? u"ril.emergency.protocol.sim1"_ns
                                  : u"ril.emergency.protocol.sim2"_ns;
  sSettingRilSuplRoamingProtocol = aServiceId == 0
                                       ? u"ril.supl.roaming_protocol.sim1"_ns
                                       : u"ril.supl.roaming_protocol.sim2"_ns;
  sSettingRilSuplEsRoamingProtocol =
      aServiceId == 0 ? u"ril.emergency.roaming_protocol.sim1"_ns
                      : u"ril.emergency.roaming_protocol.sim2"_ns;
  settings->Get(sSettingRilDataApn, this);
  settings->AddObserver(sSettingRilDataApn, this, this);
  settings->Get(sSettingRilSuplApn, this);
  settings->AddObserver(sSettingRilSuplApn, this, this);
  settings->Get(sSettingRilSuplEsApn, this);
  settings->AddObserver(sSettingRilSuplEsApn, this, this);
}

void GonkGPSGeolocationProvider::UpdateRadioInterface() {
  DBG("Get mRadioInterface with service id: %u", mRilDataServiceId);
  nsCOMPtr<nsIRadioInterfaceLayer> ril = do_GetService("@mozilla.org/ril;1");
  NS_ENSURE_TRUE_VOID(ril);
  ril->GetRadioInterface(mRilDataServiceId, getter_AddRefs(mRadioInterface));
}

bool GonkGPSGeolocationProvider::IsValidRilServiceId(uint32_t aServiceId) {
  return aServiceId < mNumberOfRilServices;
}

void GonkGPSGeolocationProvider::SetupAGPS() {
  MOZ_ASSERT(NS_IsMainThread());

  if (!mSupportsMSA && !mSupportsMSB) {
    LOG("GPS HAL supports neither MSA nor MSB");
    return;
  }

  mNumberOfRilServices = Preferences::GetUint(kPrefRilNumRadioInterfaces, 1);

  // Request RIL date service ID for correct RadioInterface object first due to
  // multi-SIM case needs it to handle AGPS related stuffs. For single SIM, 0
  // will be returned as default RIL data service ID.
  nsCOMPtr<nsISettingsManager> settings =
      do_GetService("@mozilla.org/sidl-native/settings;1");
  if (settings) {
    settings->Get(kSettingRilDefaultServiceId, this);
  }

  nsAutoCString suplServer;
  Preferences::GetCString("geo.gps.supl_server", suplServer);
  int32_t suplPort = Preferences::GetInt("geo.gps.supl_port", -1);
  if (!suplServer.IsEmpty() && suplPort > 0) {
    DBG("mAGnssHal_V2_0->set_server(%s, %d)", suplServer.get(), suplPort);
    auto result = mAGnssHal_V2_0->setServer(
        IAGnssCallback_V2_0::AGnssType::SUPL,
        std::string(suplServer.get(), suplServer.Length()), suplPort);
    if (!result.isOk() || !result) {
      ERR("failed to set server for IAGnssHal");
    }
  } else {
    DBG("Preference of SUPL server is not found");
    return;
  }
}

int32_t GonkGPSGeolocationProvider::GetDataConnectionState(
    bool isEmergencySupl) {
  if (!mRadioInterface) {
    return nsINetworkInfo::NETWORK_STATE_UNKNOWN;
  }

  int32_t state;
  int type = isEmergencySupl ? nsINetworkInfo::NETWORK_TYPE_MOBILE_ECC
                             : nsINetworkInfo::NETWORK_TYPE_MOBILE_SUPL;
  nsresult rv = mRadioInterface->GetDataCallStateByType(type, &state);

  if (NS_FAILED(rv)) {
    ERR("Failed to get SUPL data call state.");
    return nsINetworkInfo::NETWORK_STATE_UNKNOWN;
  }
  return state;
}

void GonkGPSGeolocationProvider::AGpsDataConnectionOpen(bool isEmergencySupl) {
  uint64_t netHandle =
      GetNetHandle(isEmergencySupl ? mSuplEsNetId : mSuplNetId);

  nsCString& apn = isEmergencySupl ? sRilSuplEsApn : sRilSuplApn;

  IAGnss_V2_0::ApnIpType ipType;
  if (IsRoaming()) {
    ipType = isEmergencySupl ? sIpTypeSuplEsRoaming : sIpTypeSuplRoaming;
  } else {
    ipType = isEmergencySupl ? sIpTypeSuplEs : sIpTypeSupl;
  }

  LOG("mAGnssHal_V2_0->data_conn_open_with_apn_ip_type(%llu, %s, %hhu)"
      ", netId: %d",
      netHandle, apn.get(), ipType, mSuplNetId);

  auto result = mAGnssHal_V2_0->dataConnOpen(
      netHandle, std::string(apn.get(), apn.Length()), ipType);

  if (!result.isOk() || !result) {
    ERR("failed to set APN and its IP type to IAGnss");
  }
}

void GonkGPSGeolocationProvider::HandleAGpsDataConnection(
    nsISupports* aNetworkInfo) {
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsIRilNetworkInfo> rilInfo = do_QueryInterface(aNetworkInfo);
  if (!rilInfo) {
    return;
  }

  int32_t type;
  rilInfo->GetType(&type);
  if (type != nsINetworkInfo::NETWORK_TYPE_MOBILE_SUPL &&
      type != nsINetworkInfo::NETWORK_TYPE_MOBILE_ECC) {
    return;
  }

  int32_t state;
  rilInfo->GetState(&state);
  if (state == nsINetworkInfo::NETWORK_STATE_CONNECTED) {
    int32_t netId;
    rilInfo->GetNetId(&netId);
    // Update the net id of SUPL/ECC data call
    if (type == nsINetworkInfo::NETWORK_TYPE_MOBILE_SUPL) {
      mSuplNetId = netId;
      AGpsDataConnectionOpen(false);
    } else {  // nsINetworkInfo::NETWORK_TYPE_MOBILE_ECC
      mSuplEsNetId = netId;
      AGpsDataConnectionOpen(true);
    }
  } else if (state == nsINetworkInfo::NETWORK_STATE_DISCONNECTED) {
    LOG("mAGnssHal_V2_0->data_conn_closed()");
    auto result = mAGnssHal_V2_0->dataConnClosed();
    if (!result.isOk() || !result) {
      ERR("failed to close IAGnss data connection");
    }
    if (type == nsINetworkInfo::NETWORK_TYPE_MOBILE_SUPL) {
      mSuplNetId = 0;
    } else {
      mSuplEsNetId = 0;
    }
  }
}

void GonkGPSGeolocationProvider::RequestDataConnection(bool isEmergencySupl) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!mRadioInterface) {
    ERR("mRadioInterface is null in RequestDataConnection.");
    return;
  }

  if (GetDataConnectionState(isEmergencySupl) !=
      nsINetworkInfo::NETWORK_STATE_CONNECTED) {
    LOG("nsIRadioInterface->SetupDataCallByType()");

    int32_t type = isEmergencySupl ? nsINetworkInfo::NETWORK_TYPE_MOBILE_ECC
                                   : nsINetworkInfo::NETWORK_TYPE_MOBILE_SUPL;
    nsresult rv = mRadioInterface->SetupDataCallByType(type);

    if (NS_FAILED(rv)) {
      ERR("Failed to setup SUPL data call.");
    }
  } else {
    LOG("SUPL has already connected");
    // Ideally, HAL should not request a connection when it's already connected.
    // But we still call dataConnOpen here as an error-tolerant design.
    AGpsDataConnectionOpen(isEmergencySupl);
  }
}

void GonkGPSGeolocationProvider::ReleaseDataConnection(bool isEmergencySupl) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!mRadioInterface) {
    ERR("mRadioInterface is null in ReleaseDataConnection.");
    return;
  }

  LOG("nsIRadioInterface->DeactivateDataCallByType()");
  int32_t type = isEmergencySupl ? nsINetworkInfo::NETWORK_TYPE_MOBILE_ECC
                                 : nsINetworkInfo::NETWORK_TYPE_MOBILE_SUPL;
  nsresult rv = mRadioInterface->DeactivateDataCallByType(type);

  if (NS_FAILED(rv)) {
    ERR("Failed to deactivate SUPL data call.");
  }
}

IAGnss_V2_0::ApnIpType GonkGPSGeolocationProvider::GetApnIpType(
    nsAString& protocol) {
  if (protocol.EqualsLiteral("\"IPV4V6\"")) {
    return IAGnss_V2_0::ApnIpType::IPV4V6;
  } else if (protocol.EqualsLiteral("\"IPV6\"")) {
    return IAGnss_V2_0::ApnIpType::IPV6;
  } else if (protocol.EqualsLiteral("\"IP\"")) {
    return IAGnss_V2_0::ApnIpType::IPV4;
  } else {
    return IAGnss_V2_0::ApnIpType::IPV4V6;
  }
}

bool GonkGPSGeolocationProvider::IsRoaming() {
  nsCOMPtr<nsIMobileConnectionService> service =
      do_GetService(NS_MOBILE_CONNECTION_SERVICE_CONTRACTID);
  if (!service) {
    return false;
  }

  nsCOMPtr<nsIMobileConnection> connection;
  service->GetItemByServiceId(mRilDataServiceId, getter_AddRefs(connection));
  if (!connection) {
    return false;
  }

  nsCOMPtr<nsIMobileConnectionInfo> data;
  connection->GetData(getter_AddRefs(data));
  if (!data) {
    return false;
  }
  bool roaming = false;
  data->GetRoaming(&roaming);
  return roaming;
}

NS_IMETHODIMP GonkGPSGeolocationProvider::CallStateChanged(
    uint32_t aLength, nsITelephonyCallInfo** aAllInfo) {
  bool isInEmergencySession = false;
  for (uint32_t i = 0; i < aLength; ++i) {
    nsITelephonyCallInfo* info = aAllInfo[i];
    if (info) {
      bool isEmergency = false;
      info->GetIsEmergency(&isEmergency);
      if (isEmergency) {
        uint16_t callState;
        info->GetCallState(&callState);

        if (callState != nsITelephonyService::CALL_STATE_DISCONNECTED) {
          isInEmergencySession = true;
          break;
        }
      }
    }
  }
  gIsInEmergencySession = isInEmergencySession;

  return NS_OK;
}

// The caller must avoid double registering/unregistering listener.
// Currenly, the ListenTelephonyService only called by constructor / destructor.
void GonkGPSGeolocationProvider::ListenTelephonyService(bool aStart) {
  nsCOMPtr<nsITelephonyService> service =
      do_GetService(TELEPHONY_SERVICE_CONTRACTID);
  NS_ENSURE_TRUE_VOID(service);

  nsresult rv;
  if (aStart) {
    rv = service->RegisterListener(this);
  } else {
    rv = service->UnregisterListener(this);
  }
  if (NS_FAILED(rv)) {
    ERR("Failed to register/unregister telephony service");
  }
}
#endif  // MOZ_B2G_RIL
