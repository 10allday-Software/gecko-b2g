/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/CompositorVsyncScheduler.h"

#include <stdio.h>        // for fprintf, stdout
#include <stdint.h>       // for uint64_t
#include "base/task.h"    // for CancelableTask, etc
#include "base/thread.h"  // for Thread
#include "gfxPlatform.h"  // for gfxPlatform
#ifdef MOZ_WIDGET_GTK
#  include "gfxPlatformGtk.h"  // for gfxPlatform
#endif
#include "mozilla/AutoRestore.h"  // for AutoRestore
#include "mozilla/DebugOnly.h"    // for DebugOnly
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_layers.h"
#include "mozilla/gfx/2D.h"     // for DrawTarget
#include "mozilla/gfx/Point.h"  // for IntSize
#include "mozilla/gfx/Rect.h"   // for IntSize
#include "mozilla/layers/CompositorThread.h"
#include "mozilla/layers/CompositorVsyncSchedulerOwner.h"
#include "mozilla/mozalloc.h"  // for operator new, etc
#include "nsCOMPtr.h"          // for already_AddRefed
#include "nsDebug.h"           // for NS_ASSERTION, etc
#include "nsISupportsImpl.h"   // for MOZ_COUNT_CTOR, etc
#include "nsIWidget.h"         // for nsIWidget
#include "nsThreadUtils.h"     // for NS_IsMainThread
#include "mozilla/Telemetry.h"
#include "mozilla/VsyncDispatcher.h"
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
#  include "VsyncSource.h"
#endif
#include "mozilla/widget/CompositorWidget.h"
#include "VRManager.h"

#ifdef MOZ_WIDGET_GONK
#include "GeckoTouchDispatcher.h"
#include "ScreenHelperGonk.h"
#endif

namespace mozilla {

namespace layers {

using namespace mozilla::gfx;

CompositorVsyncScheduler::Observer::Observer(CompositorVsyncScheduler* aOwner)
    : mMutex("CompositorVsyncScheduler.Observer.Mutex"), mOwner(aOwner) {}

CompositorVsyncScheduler::Observer::~Observer() { MOZ_ASSERT(!mOwner); }

bool CompositorVsyncScheduler::Observer::NotifyVsync(const VsyncEvent& aVsync) {
  MutexAutoLock lock(mMutex);
  if (!mOwner) {
    return false;
  }
  return mOwner->NotifyVsync(aVsync);
}

void CompositorVsyncScheduler::Observer::Destroy() {
  MutexAutoLock lock(mMutex);
  mOwner = nullptr;
}

CompositorVsyncScheduler::CompositorVsyncScheduler(
    CompositorVsyncSchedulerOwner* aVsyncSchedulerOwner,
    widget::CompositorWidget* aWidget)
    : CompositorScheduler(aVsyncSchedulerOwner),
      mIsObservingVsync(false),
      mVsyncNotificationsSkipped(0),
      mWidget(aWidget),
      mCurrentCompositeTaskMonitor("CurrentCompositeTaskMonitor"),
      mCurrentCompositeTask(nullptr),
      mCurrentVRTaskMonitor("CurrentVRTaskMonitor"),
      mCurrentVRTask(nullptr)
      , mSetNeedsCompositeMonitor("SetNeedsCompositeMonitor")
      , mSetNeedsCompositeTask(nullptr)
#ifdef MOZ_WIDGET_GONK
      , mDisplayEnabled(false)
      , mSetDisplayMonitor("SetDisplayMonitor")
      , mSetDisplayTask(nullptr)
#endif
{
  mVsyncObserver = new Observer(this);
#ifdef MOZ_WIDGET_GONK
  NS_DispatchToMainThread(NewRunnableMethod(
    "CompositorVsyncScheduler::SetUpDisplay", this,
    &CompositorVsyncScheduler::SetUpDisplay));
#endif

  // mAsapScheduling is set on the main thread during init,
  // but is only accessed after on the compositor thread.
  mAsapScheduling =
      StaticPrefs::layers_offmainthreadcomposition_frame_rate() == 0 ||
      gfxPlatform::IsInLayoutAsapMode();
}

CompositorVsyncScheduler::~CompositorVsyncScheduler() {
  MOZ_ASSERT(!mIsObservingVsync);
  MOZ_ASSERT(!mVsyncObserver);
  // The CompositorVsyncDispatcher is cleaned up before this in the
  // nsBaseWidget, which stops vsync listeners
  mVsyncSchedulerOwner = nullptr;
}

#ifdef MOZ_WIDGET_GONK

void
CompositorVsyncScheduler::SetUpDisplay() {
  MOZ_ASSERT(NS_IsMainThread());
  mDisplayEnabled = hal::GetScreenEnabled();
  GeckoTouchDispatcher::GetInstance()->SetCompositorVsyncScheduler(this);
  widget::ScreenHelperGonk *screenHelper = widget::ScreenHelperGonk::GetSingleton();
  screenHelper->SetCompositorVsyncScheduler(this);
}

void
CompositorVsyncScheduler::SetDisplay(bool aDisplayEnable)
{
  // SetDisplay() is usually called from nsScreenManager at main thread. Post
  // to compositor thread if needs.
  if (!CompositorThreadHolder::IsInCompositorThread()) {
    MOZ_ASSERT(NS_IsMainThread());
    MonitorAutoLock lock(mSetDisplayMonitor);
    if (mSetDisplayTask == nullptr) {
      RefPtr<CancelableRunnable> task =
          NewCancelableRunnableMethod<bool>(
              "layers::CompositorVsyncScheduler::SetDisplay", this,
              &CompositorVsyncScheduler::SetDisplay, aDisplayEnable);
      mSetDisplayTask = task;
      CompositorThread()->Dispatch(task.forget());
    }
    return;
  }

  {
    MonitorAutoLock lock(mSetDisplayMonitor);
    mSetDisplayTask = nullptr;
  }

  if (mDisplayEnabled == aDisplayEnable) {
    return;
  }

  mDisplayEnabled = aDisplayEnable;
  if (!mDisplayEnabled) {
    CancelCurrentSetNeedsCompositeTask();
    CancelCurrentCompositeTask();
  }
}

void
CompositorVsyncScheduler::CancelSetDisplayTask()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  MonitorAutoLock lock(mSetDisplayMonitor);
  if (mSetDisplayTask) {
    mSetDisplayTask->Cancel();
    mSetDisplayTask = nullptr;
  }

  // CancelSetDisplayTask is only be called in clean-up process, so
  // mDisplayEnabled could be false there.
  mDisplayEnabled = false;
}
#endif //MOZ_WIDGET_GONK

void CompositorVsyncScheduler::Destroy() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  if (!mVsyncObserver) {
    // Destroy was already called on this object.
    return;
  }
  UnobserveVsync();
  mVsyncObserver->Destroy();
  mVsyncObserver = nullptr;

  mCompositeRequestedAt = TimeStamp();
#ifdef MOZ_WIDGET_GONK
  CancelSetDisplayTask();
#endif
  CancelCurrentSetNeedsCompositeTask();
  CancelCurrentCompositeTask();
  CancelCurrentVRTask();
}

void CompositorVsyncScheduler::PostCompositeTask(
    const VsyncEvent& aVsyncEvent) {
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  if (mCurrentCompositeTask == nullptr && CompositorThread()) {
    RefPtr<CancelableRunnable> task = NewCancelableRunnableMethod<VsyncEvent>(
        "layers::CompositorVsyncScheduler::Composite", this,
        &CompositorVsyncScheduler::Composite, aVsyncEvent);
    mCurrentCompositeTask = task;
    CompositorThread()->Dispatch(task.forget());
  }
}

void CompositorVsyncScheduler::PostVRTask(TimeStamp aTimestamp) {
  MonitorAutoLock lockVR(mCurrentVRTaskMonitor);
  if (mCurrentVRTask == nullptr && CompositorThread()) {
    RefPtr<CancelableRunnable> task = NewCancelableRunnableMethod<TimeStamp>(
        "layers::CompositorVsyncScheduler::DispatchVREvents", this,
        &CompositorVsyncScheduler::DispatchVREvents, aTimestamp);
    mCurrentVRTask = task;
    CompositorThread()->Dispatch(task.forget());
  }
}

void CompositorVsyncScheduler::ScheduleComposition() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mVsyncObserver) {
    // Destroy was already called on this object.
    return;
  }

  // Make a synthetic vsync event for the calls to PostCompositeTask below.
  TimeStamp vsyncTime = TimeStamp::Now();
  TimeStamp outputTime = vsyncTime + mVsyncSchedulerOwner->GetVsyncInterval();
  VsyncEvent vsyncEvent(VsyncId(), vsyncTime, outputTime);

  if (mAsapScheduling) {
    // Used only for performance testing purposes, and when recording/replaying
    // to ensure that graphics are up to date.
    PostCompositeTask(vsyncEvent);
  } else {
    if (!mCompositeRequestedAt) {
      mCompositeRequestedAt = TimeStamp::Now();
    }
    if (!mIsObservingVsync && mCompositeRequestedAt) {
      ObserveVsync();
      // Starting to observe vsync is an async operation that goes
      // through the main thread of the UI process. It's possible that
      // we're blocking there waiting on a composite, so schedule an initial
      // one now to get things started.
      PostCompositeTask(vsyncEvent);
    }
  }
}

void
CompositorVsyncScheduler::CancelCurrentSetNeedsCompositeTask()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  MonitorAutoLock lock(mSetNeedsCompositeMonitor);
  if (mSetNeedsCompositeTask) {
    mSetNeedsCompositeTask->Cancel();
    mSetNeedsCompositeTask = nullptr;
  }
  mCompositeRequestedAt = TimeStamp();
}

/**
 * TODO Potential performance heuristics:
 * If a composite takes 17 ms, do we composite ASAP or wait until next vsync?
 * If a layer transaction comes after vsync, do we composite ASAP or wait until
 * next vsync?
 * How many skipped vsync events until we stop listening to vsync events?
 */
void
CompositorVsyncScheduler::SetNeedsComposite()
{
  if (!CompositorThreadHolder::IsInCompositorThread()) {
    MonitorAutoLock lock(mSetNeedsCompositeMonitor);
    RefPtr<CancelableRunnable> task =
        NewCancelableRunnableMethod(
            "layers::CompositorVsyncScheduler::SetNeedsComposite", this,
            &CompositorVsyncScheduler::SetNeedsComposite);
    mSetNeedsCompositeTask = task;
    CompositorThread()->Dispatch(task.forget());
    return;
  }

  {
    MonitorAutoLock lock(mSetNeedsCompositeMonitor);
    mSetNeedsCompositeTask = nullptr;
  }

#ifdef MOZ_WIDGET_GONK
  // Skip composition when display off.
  if (!mDisplayEnabled) {
    return;
  }
#endif

  mCompositeRequestedAt = TimeStamp::Now();
  if (!mIsObservingVsync && mCompositeRequestedAt) {
    ObserveVsync();
  }
}

bool CompositorVsyncScheduler::NotifyVsync(const VsyncEvent& aVsync) {
  // Called from the vsync dispatch thread. When in the GPU Process, that's
  // the same as the compositor thread.
#ifdef DEBUG
#  ifdef MOZ_WAYLAND
  // On Wayland, we dispatch vsync from the main thread, without a GPU process.
  // To allow this, we skip the following asserts if we're currently utilizing
  // the Wayland backend. The IsParentProcess guard is needed to ensure that
  // we don't accidentally attempt to initialize the gfxPlatform in the GPU
  // process on X11.
  if (!XRE_IsParentProcess() ||
      !gfxPlatformGtk::GetPlatform()->IsWaylandDisplay())
#  endif  // MOZ_WAYLAND
  {
    MOZ_ASSERT_IF(XRE_IsParentProcess(),
                  !CompositorThreadHolder::IsInCompositorThread());
    MOZ_ASSERT(!NS_IsMainThread());
  }

  MOZ_ASSERT_IF(XRE_GetProcessType() == GeckoProcessType_GPU,
                CompositorThreadHolder::IsInCompositorThread());
#endif  // DEBUG

#if defined(MOZ_WIDGET_ANDROID)
  gfx::VRManager* vm = gfx::VRManager::Get();
  if (!vm->IsPresenting()) {
    PostCompositeTask(aVsync);
  }
#else
  PostCompositeTask(aVsync);
#endif  // defined(MOZ_WIDGET_ANDROID)

  PostVRTask(aVsync.mTime);
  return true;
}

void CompositorVsyncScheduler::CancelCurrentVRTask() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread() ||
             NS_IsMainThread());
  MonitorAutoLock lock(mCurrentVRTaskMonitor);
  if (mCurrentVRTask) {
    mCurrentVRTask->Cancel();
    mCurrentVRTask = nullptr;
  }
}

void CompositorVsyncScheduler::CancelCurrentCompositeTask() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread() ||
             NS_IsMainThread());
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  if (mCurrentCompositeTask) {
    mCurrentCompositeTask->Cancel();
    mCurrentCompositeTask = nullptr;
  }
}

void CompositorVsyncScheduler::Composite(const VsyncEvent& aVsyncEvent) {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  MOZ_ASSERT(mVsyncSchedulerOwner);

  {  // scope lock
    MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
    mCurrentCompositeTask = nullptr;
  }

  mLastVsyncTime = aVsyncEvent.mTime;
  mLastVsyncOutputTime = aVsyncEvent.mOutputTime;
  mLastVsyncId = aVsyncEvent.mId;

  if (!mAsapScheduling) {
    // Some early exit conditions if we're not in ASAP mode
    if (aVsyncEvent.mTime < mLastComposeTime.Time()) {
      // We can sometimes get vsync timestamps that are in the past
      // compared to the last compose with force composites.
      // In those cases, wait until the next vsync;
      return;
    }

    if (mVsyncSchedulerOwner->IsPendingComposite()) {
      // If previous composite is still on going, finish it and wait for the
      // next vsync.
      mVsyncSchedulerOwner->FinishPendingComposite();
      return;
    }
  }
  NS_DispatchToMainThread(NewRunnableMethod<TimeStamp>(
    "CompositorVsyncScheduler::DispatchTouchEvents", this,
    &CompositorVsyncScheduler::DispatchTouchEvents, aVsyncEvent.mTime));

  DispatchVREvents(aVsyncEvent.mTime);

  if (mCompositeRequestedAt || mAsapScheduling) {
    mCompositeRequestedAt = TimeStamp();
    mLastComposeTime = SampleTime::FromVsync(aVsyncEvent.mTime);

    // Tell the owner to do a composite
    mVsyncSchedulerOwner->CompositeToTarget(aVsyncEvent.mId, nullptr, nullptr);

    mVsyncNotificationsSkipped = 0;

    TimeDuration compositeFrameTotal = TimeStamp::Now() - aVsyncEvent.mTime;
    mozilla::Telemetry::Accumulate(
        mozilla::Telemetry::COMPOSITE_FRAME_ROUNDTRIP_TIME,
        compositeFrameTotal.ToMilliseconds());
  } else if (mVsyncNotificationsSkipped++ >
             StaticPrefs::gfx_vsync_compositor_unobserve_count_AtStartup()) {
    UnobserveVsync();
  }
}

void CompositorVsyncScheduler::ForceComposeToTarget(gfx::DrawTarget* aTarget,
                                                    const IntRect* aRect) {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  /**
   * bug 1138502 - There are cases such as during long-running window resizing
   * events where we receive many force-composites. We also continue to get
   * vsync notifications. Because the force-composites trigger compositing and
   * clear the mCompositeRequestedAt timestamp, the vsync notifications will not
   * need to do anything and so will increment the mVsyncNotificationsSkipped
   * counter to indicate the vsync was ignored. If this happens enough times, we
   * will disable listening for vsync entirely. On the next force-composite we
   * will enable listening for vsync again, and continued force-composites and
   * vsyncs will cause oscillation between observing vsync and not. On some
   * platforms, enabling/disabling vsync is not free and this oscillating
   * behavior causes a performance hit. In order to avoid this problem, we reset
   * the mVsyncNotificationsSkipped counter to keep vsync enabled.
   */
  mVsyncNotificationsSkipped = 0;

  mLastComposeTime = SampleTime::FromNow();
  MOZ_ASSERT(mVsyncSchedulerOwner);
  mVsyncSchedulerOwner->CompositeToTarget(VsyncId(), aTarget, aRect);
}

bool CompositorVsyncScheduler::NeedsComposite() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  return (bool)mCompositeRequestedAt;
}

bool CompositorVsyncScheduler::FlushPendingComposite() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (mCompositeRequestedAt) {
    CancelCurrentCompositeTask();
    ForceComposeToTarget(nullptr, nullptr);
    return true;
  }
  return false;
}

void CompositorVsyncScheduler::ObserveVsync() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mWidget->ObserveVsync(mVsyncObserver);
  mIsObservingVsync = true;
}

void CompositorVsyncScheduler::UnobserveVsync() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mWidget->ObserveVsync(nullptr);
  mIsObservingVsync = false;
}

void
CompositorVsyncScheduler::DispatchTouchEvents(TimeStamp aVsyncTimestamp)
{
#ifdef MOZ_WIDGET_GONK
  GeckoTouchDispatcher::GetInstance()->NotifyVsync(aVsyncTimestamp);
#endif
}

void CompositorVsyncScheduler::DispatchVREvents(TimeStamp aVsyncTimestamp) {
  {
    MonitorAutoLock lock(mCurrentVRTaskMonitor);
    mCurrentVRTask = nullptr;
  }
  // This only allows to be called by CompositorVsyncScheduler::PostVRTask()
  // When the process is going to shutdown, the runnable has chance to be
  // executed by other threads, we only want it to be run in the compositor
  // thread.
  if (!CompositorThreadHolder::IsInCompositorThread()) {
    return;
  }

  VRManager* vm = VRManager::Get();
  vm->NotifyVsync(aVsyncTimestamp);
}

}  // namespace layers
}  // namespace mozilla
