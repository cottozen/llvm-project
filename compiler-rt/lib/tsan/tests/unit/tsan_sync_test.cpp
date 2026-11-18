//===-- tsan_sync_test.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//
#include <stdlib.h>

#include "tsan_sync.h"
#include "tsan_rtl.h"
#include "gtest/gtest.h"

namespace __tsan {

TEST(MetaMap, Basic) {
  ThreadState *thr = cur_thread();
  SlotLocker locker(thr);
  MetaMap *m = &ctx->metamap;
  u64 block[1] = {};  // fake malloc block
  m->AllocBlock(thr, 0, (uptr)&block[0], 1 * sizeof(u64));
  MBlock *mb = m->GetBlock((uptr)&block[0]);
  CHECK_NE(mb, (MBlock *)0);
  CHECK_EQ(mb->siz, 1 * sizeof(u64));
  CHECK_EQ(mb->tid, thr->tid);
  uptr sz = m->FreeBlock(thr->proc(), (uptr)&block[0], true);
  CHECK_EQ(sz, 1 * sizeof(u64));
  mb = m->GetBlock((uptr)&block[0]);
  CHECK_EQ(mb, (MBlock *)0);
}

TEST(MetaMap, FreeRange) {
  ThreadState *thr = cur_thread();
  SlotLocker locker(thr);
  MetaMap *m = &ctx->metamap;
  u64 block[4] = {};  // fake malloc block
  m->AllocBlock(thr, 0, (uptr)&block[0], 1 * sizeof(u64));
  m->AllocBlock(thr, 0, (uptr)&block[1], 3 * sizeof(u64));
  MBlock *mb1 = m->GetBlock((uptr)&block[0]);
  CHECK_EQ(mb1->siz, 1 * sizeof(u64));
  MBlock *mb2 = m->GetBlock((uptr)&block[1]);
  CHECK_EQ(mb2->siz, 3 * sizeof(u64));
  m->FreeRange(thr->proc(), (uptr)&block[0], 4 * sizeof(u64), true);
  mb1 = m->GetBlock((uptr)&block[0]);
  CHECK_EQ(mb1, (MBlock *)0);
  mb2 = m->GetBlock((uptr)&block[1]);
  CHECK_EQ(mb2, (MBlock *)0);
}

TEST(MetaMap, Sync) {
  // CHECK can call memset/etc. Disable interceptors to prevent
  // them from detecting that we exit runtime with mutexes held.
  ScopedIgnoreInterceptors ignore;
  ThreadState *thr = cur_thread();
  SlotLocker locker(thr);
  MetaMap *m = &ctx->metamap;
  u64 block[4] = {};  // fake malloc block
  m->AllocBlock(thr, 0, (uptr)&block[0], 4 * sizeof(u64));
  SyncVar *s1 = m->GetSyncIfExists((uptr)&block[0]);
  CHECK_EQ(s1, (SyncVar *)0);
  s1 = m->GetSyncOrCreate(thr, 0, (uptr)&block[0], false);
  CHECK_NE(s1, (SyncVar *)0);
  CHECK_EQ(s1->addr, (uptr)&block[0]);
  SyncVar *s2 = m->GetSyncOrCreate(thr, 0, (uptr)&block[1], false);
  CHECK_NE(s2, (SyncVar *)0);
  CHECK_EQ(s2->addr, (uptr)&block[1]);
  m->FreeBlock(thr->proc(), (uptr)&block[0], true);
  s1 = m->GetSyncIfExists((uptr)&block[0]);
  CHECK_EQ(s1, (SyncVar *)0);
  s2 = m->GetSyncIfExists((uptr)&block[1]);
  CHECK_EQ(s2, (SyncVar *)0);
  m->OnProcIdle(thr->proc());
}

TEST(MetaMap, MoveMemory) {
  ScopedIgnoreInterceptors ignore;
  ThreadState *thr = cur_thread();
  SlotLocker locker(thr);
  MetaMap *m = &ctx->metamap;
  u64 block1[4] = {};  // fake malloc block
  u64 block2[4] = {};  // fake malloc block
  m->AllocBlock(thr, 0, (uptr)&block1[0], 3 * sizeof(u64));
  m->AllocBlock(thr, 0, (uptr)&block1[3], 1 * sizeof(u64));
  SyncVar *s1 = m->GetSyncOrCreate(thr, 0, (uptr)&block1[0], false);
  SyncVar *s2 = m->GetSyncOrCreate(thr, 0, (uptr)&block1[1], false);
  m->MoveMemory((uptr)&block1[0], (uptr)&block2[0], 4 * sizeof(u64));
  MBlock *mb1 = m->GetBlock((uptr)&block1[0]);
  CHECK_EQ(mb1, (MBlock *)0);
  MBlock *mb2 = m->GetBlock((uptr)&block1[3]);
  CHECK_EQ(mb2, (MBlock *)0);
  mb1 = m->GetBlock((uptr)&block2[0]);
  CHECK_NE(mb1, (MBlock *)0);
  CHECK_EQ(mb1->siz, 3 * sizeof(u64));
  mb2 = m->GetBlock((uptr)&block2[3]);
  CHECK_NE(mb2, (MBlock *)0);
  CHECK_EQ(mb2->siz, 1 * sizeof(u64));
  s1 = m->GetSyncIfExists((uptr)&block1[0]);
  CHECK_EQ(s1, (SyncVar *)0);
  s2 = m->GetSyncIfExists((uptr)&block1[1]);
  CHECK_EQ(s2, (SyncVar *)0);
  s1 = m->GetSyncIfExists((uptr)&block2[0]);
  CHECK_NE(s1, (SyncVar *)0);
  CHECK_EQ(s1->addr, (uptr)&block2[0]);
  s2 = m->GetSyncIfExists((uptr)&block2[1]);
  CHECK_NE(s2, (SyncVar *)0);
  CHECK_EQ(s2->addr, (uptr)&block2[1]);
  m->FreeRange(thr->proc(), (uptr)&block2[0], 4 * sizeof(u64), true);
}

TEST(MetaMap, ResetSync) {
  ScopedIgnoreInterceptors ignore;
  ThreadState *thr = cur_thread();
  SlotLocker locker(thr);
  MetaMap *m = &ctx->metamap;
  u64 block[1] = {};  // fake malloc block
  m->AllocBlock(thr, 0, (uptr)&block[0], 1 * sizeof(u64));
  SyncVar *s = m->GetSyncOrCreate(thr, 0, (uptr)&block[0], false);
  s->Reset();
  uptr sz = m->FreeBlock(thr->proc(), (uptr)&block[0], true);
  CHECK_EQ(sz, 1 * sizeof(u64));
}

TEST(MetaMap, LockMonitorLifetime) {
  ScopedIgnoreInterceptors ignore;
  ThreadState *thr = cur_thread();
  SlotLocker locker(thr);
  MetaMap *m = &ctx->metamap;
  u64 block[2] = {};
  SyncVar *first = m->GetSyncOrCreate(thr, 0, (uptr)&block[0], false);
  SyncVar *second = m->GetSyncOrCreate(thr, 0, (uptr)&block[1], false);
  EXPECT_EQ(first->monitor, nullptr);
  EXPECT_EQ(second->monitor, nullptr);
  {
    Lock lock(&first->mtx);
    Monitor *monitor = &first->GetOrCreateMonitor(thr->tid);
    EXPECT_EQ(&first->GetOrCreateMonitor(thr->tid + 1), monitor);
    first->Reset();
    EXPECT_EQ(first->monitor, nullptr);
    EXPECT_NE(&first->GetOrCreateMonitor(thr->tid), nullptr);
  }
  {
    Lock lock(&second->mtx);
    EXPECT_NE(&second->GetOrCreateMonitor(thr->tid), first->monitor);
  }
  m->FreeRange(thr->proc(), (uptr)block, sizeof(block), true);
}

TEST(MetaMap, MutexAcquisitionCreatesMonitor) {
  ScopedIgnoreInterceptors ignore;
  ThreadState *thr = cur_thread();
  u64 block[2] = {};
  // Statically initialized locks can be acquired without MutexCreate.
  MutexPostLock(thr, 0, (uptr)&block[0]);
  MutexPostReadLock(thr, 0, (uptr)&block[1]);
  {
    SlotLocker locker(thr);
    auto first = ctx->metamap.GetSyncIfExists((uptr)&block[0]);
    auto second = ctx->metamap.GetSyncIfExists((uptr)&block[1]);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->monitor, nullptr);
    EXPECT_NE(second->monitor, nullptr);
    EXPECT_NE(first->monitor, second->monitor);
  }
  MutexUnlock(thr, 0, (uptr)&block[0]);
  MutexReadUnlock(thr, 0, (uptr)&block[1]);
  MutexDestroy(thr, 0, (uptr)&block[0]);
  MutexDestroy(thr, 0, (uptr)&block[1]);
  SlotLocker locker(thr);
  EXPECT_EQ(ctx->metamap.GetSyncIfExists((uptr)&block[0])->monitor, nullptr);
  EXPECT_EQ(ctx->metamap.GetSyncIfExists((uptr)&block[1])->monitor, nullptr);
  ctx->metamap.FreeRange(thr->proc(), (uptr)block, sizeof(block), true);
}

TEST(MetaMap, MonitorWrappersTrackLockHandoffs) {
  ScopedIgnoreInterceptors ignore;
  ThreadState *thr = cur_thread();
  u64 block[2] = {};
  SyncVar *first_lock;
  SyncVar *second_lock;
  {
    SlotLocker locker(thr);
    first_lock = ctx->metamap.GetSyncOrCreate(thr, 0, (uptr)&block[0], false);
    second_lock = ctx->metamap.GetSyncOrCreate(thr, 0, (uptr)&block[1], false);
  }
  ThreadState first(0);
  ThreadState second(1);
  first.monitor.BeginTransaction();
  second.monitor.BeginTransaction();
  // Acquiring an unpublished lock has no incoming dependency.
  MonitorAcquire(&first, 0, first_lock);
  {
    Lock lock(&first_lock->mtx);
    MonitorRelease(&first, first_lock);
  }
  MonitorAcquire(&second, 0, first_lock);
  {
    Lock lock(&second_lock->mtx);
    MonitorRelease(&second, second_lock);
    // The return handoff closes a cycle between the active transactions.
    EXPECT_EQ(first.monitor.Acquire(*second_lock->monitor),
              AcquireResult::IncreasingCycle);
  }
  SlotLocker locker(thr);
  ctx->metamap.FreeRange(thr->proc(), (uptr)block, sizeof(block), true);
}

TEST(MetaMap, MutexAcquireReportsConflict) {
  EXPECT_EXIT(
      {
        ScopedIgnoreInterceptors ignore;
        ThreadState *thr = cur_thread();
        u64 block = 0;
        SyncVar *sync;
        {
          SlotLocker locker(thr);
          sync = ctx->metamap.GetSyncOrCreate(thr, 0, (uptr)&block, false);
        }
        thr->monitor.BeginTransaction();
        Monitor snapshot(thr->tid);
        thr->monitor.Release(snapshot);
        Monitor other(thr->tid + 1);
        other.BeginTransaction();
        other.Acquire(snapshot);
        {
          Lock lock(&sync->mtx);
          other.Release(sync->GetOrCreateMonitor(thr->tid));
        }
        MutexPostLock(thr, GET_CURRENT_PC(), (uptr)&block);
        _Exit(0);
      },
      ::testing::ExitedWithCode(0), "transaction conflict \\(increasing cycle\\)");
}

TEST(MetaMap, MutexMonitorPublishesOnFinalUnlock) {
  ScopedIgnoreInterceptors ignore;
  ThreadState *thr = cur_thread();
  u64 block = 0;
  const uptr addr = (uptr)&block;
  thr->monitor.BeginTransaction();
  MutexPostLock(thr, 0, addr);
  MutexPostLock(thr, 0, addr);
  MutexUnlock(thr, 0, addr);
  SyncVar *sync = ctx->metamap.GetSyncIfExists(addr);
  Monitor empty(thr->tid + 1);
  {
    Lock lock(&sync->mtx);
    ConflictGraph graph(empty, *sync->monitor);
    EXPECT_EQ(graph.nodes.Size(), 0u);
  }
  MutexUnlock(thr, 0, addr);
  {
    Lock lock(&sync->mtx);
    ConflictGraph graph(empty, *sync->monitor);
    EXPECT_EQ(graph.nodes.Size(), 1u);
  }
  // Both rwlock modes publish to the same lock monitor.
  Monitor *lock_monitor = sync->monitor;
  MutexPostReadLock(thr, 0, addr);
  MutexReadOrWriteUnlock(thr, 0, addr);
  {
    Lock lock(&sync->mtx);
    EXPECT_EQ(sync->monitor, lock_monitor);
    ConflictGraph graph(empty, *sync->monitor);
    EXPECT_EQ(graph.nodes.Size(), 1u);
  }
  MutexPostLock(thr, 0, addr);
  MutexReadOrWriteUnlock(thr, 0, addr);
  {
    Lock lock(&sync->mtx);
    EXPECT_EQ(sync->monitor, lock_monitor);
    ConflictGraph graph(empty, *sync->monitor);
    EXPECT_EQ(graph.nodes.Size(), 1u);
  }
  thr->monitor.EndTransaction();
  MutexDestroy(thr, 0, addr);
  SlotLocker locker(thr);
  ctx->metamap.FreeRange(thr->proc(), addr, sizeof(block), true);
}


}  // namespace __tsan
