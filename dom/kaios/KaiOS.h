/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* (c) 2019 KAI OS TECHNOLOGIES (HONG KONG) LIMITED All rights reserved. This
 * file or any portion thereof may not be reproduced or used in any manner
 * whatsoever without the express written permission of KAI OS TECHNOLOGIES
 * (HONG KONG) LIMITED. KaiOS is the trademark of KAI OS TECHNOLOGIES (HONG
 * KONG) LIMITED or its affiliate company and may be registered in some
 * jurisdictions. All other trademarks are the property of their respective
 * owners.
 */

#ifndef mozilla_dom_KaiOS_h
#define mozilla_dom_KaiOS_h

#include "mozilla/dom/BindingDeclarations.h"
#ifdef HAS_KOOST_MODULES
#  include "mozilla/dom/ExternalAPI.h"
#endif
#ifdef MOZ_B2G_BT
#  include "mozilla/dom/bluetooth/BluetoothManager.h"
#endif
#ifdef MOZ_B2G_RIL
#  include "mozilla/dom/Telephony.h"
#  include "mozilla/dom/MobileConnectionArray.h"
#endif
#ifndef DISABLE_WIFI
#  include "mozilla/dom/WifiManagerBinding.h"
#endif

#include "mozilla/dom/TetheringManagerBinding.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/MemoryReporting.h"
#include "nsPIDOMWindow.h"
#include "nsWrapperCache.h"

//*****************************************************************************
// KaiOS: Script "kaiOS" object
//*****************************************************************************

namespace mozilla {
namespace dom {

class KaiOS final : public nsISupports, public nsWrapperCache {
 public:
  explicit KaiOS(nsPIDOMWindowInner* aInnerWindow);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(KaiOS)

  nsPIDOMWindowInner* GetWindow() const { return mWindow; }
  nsPIDOMWindowInner* GetParentObject() const { return GetWindow(); }

  void Invalidate();
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

  virtual JSObject* WrapObject(JSContext* cx,
                               JS::Handle<JSObject*> aGivenProto) override;

#ifdef HAS_KOOST_MODULES
  ExternalAPI* GetExternalapi(ErrorResult& aRv);
#endif

#ifdef MOZ_B2G_BT
  bluetooth::BluetoothManager* GetMozBluetooth(ErrorResult& aRv);
#endif  // MOZ_B2G_BT

#ifdef MOZ_B2G_RIL
  MobileConnectionArray* GetMozMobileConnections(ErrorResult& aRv);
  Telephony* GetMozTelephony(ErrorResult& aRv);
#endif  // MOZ_B2G_RIL

  TetheringManager* GetTetheringManager(ErrorResult& aRv);

#ifndef DISABLE_WIFI
  WifiManager* GetWifiManager(ErrorResult& aRv);
#endif
  static bool HasWifiManagerSupport(JSContext* /* unused */, JSObject* aGlobal);

 private:
  virtual ~KaiOS();

  nsCOMPtr<nsPIDOMWindowInner> mWindow;
#ifdef HAS_KOOST_MODULES
  RefPtr<ExternalAPI> mExternalAPI;
#endif

#ifdef MOZ_B2G_BT
  RefPtr<bluetooth::BluetoothManager> mBluetooth;
#endif

#ifdef MOZ_B2G_RIL
  RefPtr<MobileConnectionArray> mMobileConnections;
  RefPtr<Telephony> mTelephony;
#endif
  RefPtr<TetheringManager> mTetheringManager;

#ifndef DISABLE_WIFI
  RefPtr<WifiManager> mWifiManager;
#endif
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_KaiOS_h
