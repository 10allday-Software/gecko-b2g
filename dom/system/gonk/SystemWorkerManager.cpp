/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
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

#include "SystemWorkerManager.h"
#include "mozilla/Services.h"

#if 0
#  include "nsINetworkService.h"
#  include "nsIXPConnect.h"
#endif
#include "nsIWifi.h"
#include "nsIWorkerHolder.h"

#if 0
#  include "jsfriendapi.h"
#  include "mozilla/dom/workers/Workers.h"
#endif
#include "AutoMounter.h"
#include "nsIObserverService.h"
#if 0
#  include "TimeZoneSettingObserver.h"
#  include "AudioManager.h"
#  include "mozilla/dom/ScriptSettings.h"
#  include "mozilla/ipc/KeyStore.h"
#  include "nsServiceManagerUtils.h"
#  include "nsThreadUtils.h"
#  include "mozilla/Services.h"
#endif
#include "WifiWorker.h"
#include "nsComponentManagerUtils.h"

#define WORKERS_SHUTDOWN_TOPIC "web-workers-shutdown"

using namespace mozilla::dom::gonk;
using namespace mozilla::ipc;
using namespace mozilla::system;

namespace {

NS_DEFINE_CID(kWifiWorkerCID, NS_WIFIWORKER_CID);

// Doesn't carry a reference, we're owned by services.
SystemWorkerManager* gInstance = nullptr;

}  // namespace

SystemWorkerManager::SystemWorkerManager() : mShutdown(false) {
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!gInstance, "There should only be one instance!");
}

SystemWorkerManager::~SystemWorkerManager() {
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!gInstance || gInstance == this,
               "There should only be one instance!");
  gInstance = nullptr;
}

nsresult SystemWorkerManager::Init() {
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  NS_ASSERTION(NS_IsMainThread(), "We can only initialize on the main thread");
  NS_ASSERTION(!mShutdown, "Already shutdown!");

  nsresult rv;
#ifndef DISABLE_WIFI
  rv = InitWifi();
  if (NS_FAILED(rv)) {
    NS_WARNING("Failed to initialize WiFi Networking!");
    return rv;
  }
#endif

#if 0
  InitKeyStore();
#endif

  InitAutoMounter();

#if 0
  InitializeTimeZoneSettingObserver();
  nsCOMPtr<nsIAudioManager> audioManager =
    do_GetService(NS_AUDIOMANAGER_CONTRACTID);
#endif

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (!obs) {
    NS_WARNING("Failed to get observer service!");
    return NS_ERROR_FAILURE;
  }

  rv = obs->AddObserver(this, WORKERS_SHUTDOWN_TOPIC, false);
  if (NS_FAILED(rv)) {
    NS_WARNING("Failed to initialize worker shutdown event!");
    return rv;
  }

  return NS_OK;
}

void SystemWorkerManager::Shutdown() {
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  mShutdown = true;

  ShutdownAutoMounter();

#ifndef DISABLE_WIFI
  nsCOMPtr<nsIWifi> wifi(do_QueryInterface(mWifiWorker));
  if (wifi) {
    wifi->Shutdown();
    wifi = nullptr;
  }
  mWifiWorker = nullptr;
#endif

#if 0
  if (mKeyStore) {
    mKeyStore->Shutdown();
    mKeyStore = nullptr;
  }
#endif

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->RemoveObserver(this, WORKERS_SHUTDOWN_TOPIC);
  }
}

// static
already_AddRefed<SystemWorkerManager> SystemWorkerManager::FactoryCreate() {
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  RefPtr<SystemWorkerManager> instance(gInstance);

  if (!instance) {
    instance = new SystemWorkerManager();
    if (NS_FAILED(instance->Init())) {
      instance->Shutdown();
      return nullptr;
    }

    gInstance = instance;
  }

  return instance.forget();
}

// static
nsIInterfaceRequestor* SystemWorkerManager::GetInterfaceRequestor() {
  return gInstance;
}

NS_IMETHODIMP
SystemWorkerManager::GetInterface(const nsIID& aIID, void** aResult) {
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (aIID.Equals(NS_GET_IID(nsIWifi))) {
#ifndef DISABLE_WIFI
    return CallQueryInterface(mWifiWorker,
                              reinterpret_cast<nsIWifi**>(aResult));
#else
    return NS_ERROR_NOT_IMPLEMENTED;
#endif
  }

  NS_WARNING("Got nothing for the requested IID!");
  return NS_ERROR_NO_INTERFACE;
}
nsresult SystemWorkerManager::RegisterRilWorker(unsigned int aClientId,
                                                JS::Handle<JS::Value> aWorker,
                                                JSContext* aCx) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

#ifndef DISABLE_WIFI
nsresult
SystemWorkerManager::InitWifi()
{
  nsCOMPtr<nsIWorkerHolder> worker = do_CreateInstance(kWifiWorkerCID);
  NS_ENSURE_TRUE(worker, NS_ERROR_FAILURE);

  mWifiWorker = worker;
  return NS_OK;
}
#endif

#if 0
nsresult
SystemWorkerManager::InitKeyStore()
{
  mKeyStore = new KeyStore();
  return NS_OK;
}
#endif

NS_IMPL_ISUPPORTS(SystemWorkerManager, nsIObserver, nsIInterfaceRequestor,
                  nsISystemWorkerManager)

NS_IMETHODIMP
SystemWorkerManager::Observe(nsISupports* aSubject, const char* aTopic,
                             const char16_t* aData) {
  if (!strcmp(aTopic, WORKERS_SHUTDOWN_TOPIC)) {
    Shutdown();
  }

  return NS_OK;
}
