/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/SystemMessageManager.h"

#include "mozilla/dom/SystemMessageManagerBinding.h"
#include "mozilla/dom/SystemMessageSubscription.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseWorkerProxy.h"
#include "mozilla/dom/WorkerRunnable.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerScope.h"
#include "nsIGlobalObject.h"

namespace mozilla {
namespace dom {

namespace {

class CreateSubscriptionRunnable final : public WorkerMainThreadRunnable {
 public:
  explicit CreateSubscriptionRunnable(WorkerPrivate* aWorkerPrivate,
                                      SystemMessageManager* aManager,
                                      PromiseWorkerProxy* aProxy,
                                      const nsAString& aMessageName)
      : WorkerMainThreadRunnable(aWorkerPrivate,
                                 "SystemMessage :: CreateSubscription"_ns),
        mManager(aManager),
        mProxy(aProxy),
        mMessageName(aMessageName) {}

  bool MainThreadRun() override {
    AssertIsOnMainThread();

    mError = mManager->CreateSubscription(nullptr, mProxy, mMessageName);
    return true;
  }

  nsresult mError;

 private:
  ~CreateSubscriptionRunnable() {}

  RefPtr<SystemMessageManager> mManager;
  RefPtr<PromiseWorkerProxy> mProxy;
  nsString mMessageName;
};

}  // anonymous namespace

SystemMessageManager::SystemMessageManager(nsIGlobalObject* aGlobal,
                                           const nsACString& aScope)
    : mGlobal(aGlobal), mScope(aScope) {
  AssertIsOnMainThread();
}

SystemMessageManager::SystemMessageManager(const nsACString& aScope)
    : mScope(aScope) {
#ifdef DEBUG
  // There's only one global on a worker, so we don't need to pass a global
  // object to the constructor.
  WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
  MOZ_ASSERT(worker);
  worker->AssertIsOnWorkerThread();
#endif
}

SystemMessageManager::~SystemMessageManager() {}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(SystemMessageManager, mGlobal)
NS_IMPL_CYCLE_COLLECTING_ADDREF(SystemMessageManager)
NS_IMPL_CYCLE_COLLECTING_RELEASE(SystemMessageManager)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(SystemMessageManager)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject* SystemMessageManager::WrapObject(JSContext* aCx,
                                           JS::Handle<JSObject*> aGivenProto) {
  return SystemMessageManager_Binding::Wrap(aCx, this, aGivenProto);
}

// static
already_AddRefed<SystemMessageManager> SystemMessageManager::Create(
    nsIGlobalObject* aGlobal, const nsACString& aScope, ErrorResult& aRv) {
  if (!NS_IsMainThread()) {
    RefPtr<SystemMessageManager> ret = new SystemMessageManager(aScope);
    return ret.forget();
  }

  RefPtr<SystemMessageManager> ret = new SystemMessageManager(aGlobal, aScope);

  return ret.forget();
}

already_AddRefed<Promise> SystemMessageManager::Subscribe(
    const nsAString& aMessageName, ErrorResult& aRv) {
  if (NS_IsMainThread()) {
    RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
    if (NS_WARN_IF(aRv.Failed())) {
      return nullptr;
    }

    nsresult rv = CreateSubscription(promise, nullptr, aMessageName);
    if (NS_FAILED(rv)) {
      promise->MaybeReject(rv);
      return promise.forget();
    }

    return promise.forget();
  } else {
    WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(worker);
    worker->AssertIsOnWorkerThread();

    nsCOMPtr<nsIGlobalObject> global = worker->GlobalScope();
    RefPtr<Promise> promise = Promise::Create(global, aRv);
    if (NS_WARN_IF(aRv.Failed())) {
      return nullptr;
    }

    RefPtr<PromiseWorkerProxy> proxy =
        PromiseWorkerProxy::Create(worker, promise);
    if (!proxy) {
      promise->MaybeReject(NS_ERROR_DOM_ABORT_ERR);
      return promise.forget();
    }

    RefPtr<CreateSubscriptionRunnable> r =
        new CreateSubscriptionRunnable(worker, this, proxy, aMessageName);
    ErrorResult rv;
    r->Dispatch(Canceling, rv);
    if (rv.Failed()) {
      promise->MaybeReject(NS_ERROR_DOM_ABORT_ERR);
      return promise.forget();
    }

    if (NS_FAILED(r->mError)) {
      promise->MaybeReject(r->mError);
      return promise.forget();
    }

    return promise.forget();
  }

  return nullptr;
}

nsresult SystemMessageManager::CreateSubscription(
    Promise* aPromise, PromiseWorkerProxy* aProxy,
    const nsAString& aMessageName) {
  AssertIsOnMainThread();

  // TODO: Maybe we can check for permissions from principal here.
  nsCOMPtr<nsIPrincipal> principal;
  if (mGlobal) {
    principal = mGlobal->PrincipalOrNull();
  } else if (aProxy) {
    MutexAutoLock lock(aProxy->Lock());
    if (aProxy->CleanedUp()) {
      return NS_ERROR_DOM_ABORT_ERR;
    }
    principal = aProxy->GetWorkerPrivate()->GetPrincipal();
  }

  if (!principal) {
    return NS_ERROR_DOM_ABORT_ERR;
  }

  RefPtr<SystemMessageSubscription> subscription =
      SystemMessageSubscription::Create(principal, aPromise, aProxy, mScope,
                                        aMessageName);
  return subscription->Subscribe();
}

}  // namespace dom
}  // namespace mozilla
