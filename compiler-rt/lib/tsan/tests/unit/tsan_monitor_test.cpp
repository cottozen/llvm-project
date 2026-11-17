#include "tsan_monitor.h"

#include "gtest/gtest.h"

namespace __tsan {

TEST(ConflictGraph, EmptyViews) {
  Monitor p(0);
  Monitor incoming(1);
  ConflictGraph graph(p, incoming);
  EXPECT_EQ(graph.nodes.Size(), 0u);
  EXPECT_EQ(graph.edges.Size(), 0u);
}

TEST(ConflictGraph, IndependentTransactionsHaveNoEdges) {
  Monitor p(0);
  Monitor incoming(1);
  p.BeginTransaction();
  incoming.BeginTransaction();
  ConflictGraph graph(p, incoming);
  ASSERT_EQ(graph.nodes.Size(), 2u);
  EXPECT_EQ(graph.nodes[0].view, ConflictGraph::View::Thread);
  EXPECT_EQ(graph.nodes[0].thread_id, 0u);
  EXPECT_EQ(graph.nodes[1].view, ConflictGraph::View::Lock);
  EXPECT_EQ(graph.nodes[1].thread_id, 1u);
  EXPECT_EQ(graph.edges.Size(), 0u);
}

TEST(ConflictGraph, EdgesInBothDirectionsAndDistinctViewVertices) {
  Monitor first(0);
  Monitor second(1);
  Monitor snapshot(0);
  first.BeginTransaction();
  first.Release(snapshot);
  second.BeginTransaction();
  ASSERT_EQ(second.Acquire(snapshot), AcquireResult::Ok);
  second.Release(snapshot);

  ConflictGraph graph(second, snapshot);
  ASSERT_EQ(graph.nodes.Size(), 4u);
  ASSERT_EQ(graph.edges.Size(), 2u);
  bool from_thread = false;
  bool from_incoming = false;
  for (uptr i = 0; i < graph.edges.Size(); ++i) {
    const auto& edge = graph.edges[i];
    const auto& source = graph.nodes[edge.source];
    const auto& target = graph.nodes[edge.target];
    EXPECT_EQ(source.thread_id, 0u);
    EXPECT_EQ(target.thread_id, 1u);
    EXPECT_NE(source.view, target.view);
    from_thread |= source.view == ConflictGraph::View::Thread;
    from_incoming |= source.view == ConflictGraph::View::Lock;
  }
  EXPECT_TRUE(from_thread);
  EXPECT_TRUE(from_incoming);
}

TEST(ConflictGraph, InactiveTransactionsAreNotVertices) {
  Monitor p(0);
  Monitor incoming(0);
  p.BeginTransaction();
  p.Release(incoming);
  p.EndTransaction();
  ConflictGraph graph(p, incoming);
  ASSERT_EQ(graph.nodes.Size(), 1u);
  EXPECT_EQ(graph.nodes[0].view, ConflictGraph::View::Lock);
  EXPECT_EQ(graph.edges.Size(), 0u);
}

TEST(ConflictGraph, AcquireGrowsViewsForDistantThreads) {
  Monitor first(100);
  Monitor second(0);
  Monitor snapshot(100);
  first.BeginTransaction();
  first.Release(snapshot);
  second.BeginTransaction();
  ASSERT_EQ(second.Acquire(snapshot), AcquireResult::Ok);
  second.Release(snapshot);
  ConflictGraph graph(second, snapshot);
  ASSERT_EQ(graph.nodes.Size(), 4u);
  ASSERT_EQ(graph.edges.Size(), 2u);
  for (uptr i = 0; i < graph.edges.Size(); ++i) {
    EXPECT_EQ(graph.nodes[graph.edges[i].source].thread_id, 100u);
    EXPECT_EQ(graph.nodes[graph.edges[i].target].thread_id, 0u);
  }
}

TEST(ConflictGraph, TopSortIncludesBranchesAndIsolatedVertices) {
  Monitor p(0), l(1);
  ConflictGraph graph(p, l);
  for (Tid r = 0; r < 6; ++r)
    graph.nodes.PushBack({ConflictGraph::View::Thread, r});
  graph.edges.PushBack({0, 1});
  graph.edges.PushBack({0, 2});
  graph.edges.PushBack({1, 3});
  graph.edges.PushBack({2, 3});
  graph.edges.PushBack({3, 4});

  Vector<uptr> order;
  ASSERT_TRUE(graph.TopSort(order));
  ASSERT_EQ(order.Size(), graph.nodes.Size());
  Vector<uptr> positions;
  positions.Resize(graph.nodes.Size());
  for (uptr i = 0; i < order.Size(); ++i)
    positions[order[i]] = i;
  for (uptr i = 0; i < graph.edges.Size(); ++i)
    EXPECT_LT(positions[graph.edges[i].source], positions[graph.edges[i].target]);
}

TEST(ConflictGraph, TopSortDetectsCycleWithDownstreamVertex) {
  Monitor p(0), l(1);
  ConflictGraph graph(p, l);
  for (Tid r = 0; r < 4; ++r)
    graph.nodes.PushBack({ConflictGraph::View::Thread, r});
  graph.edges.PushBack({0, 1});
  graph.edges.PushBack({1, 0});
  graph.edges.PushBack({1, 2});
  Vector<uptr> order;
  EXPECT_FALSE(graph.TopSort(order));
  ASSERT_EQ(order.Size(), 1u);
  EXPECT_EQ(order[0], 3u);
}

TEST(ConflictGraph, EmptyTopSortClearsPreviousOrder) {
  Monitor p(0), l(1);
  ConflictGraph graph(p, l);
  Vector<uptr> order;
  order.PushBack(42);
  EXPECT_TRUE(graph.TopSort(order));
  EXPECT_EQ(order.Size(), 0u);
}

TEST(ConflictGraph, TransposeGroupsPredecessorsAndKeepsEmptyRanges) {
  Monitor p(0), l(1);
  ConflictGraph G(p, l);
  for (Tid r = 0; r < 4; ++r)
    G.nodes.PushBack({ConflictGraph::View::Thread, r});
  G.edges.PushBack({0, 2});
  G.edges.PushBack({1, 2});
  G.edges.PushBack({2, 3});

  auto G_T = G.Transpose();
  EXPECT_TRUE(G_T[0].empty());
  EXPECT_TRUE(G_T[1].empty());
  ASSERT_EQ(G_T[2].size(), 2u);
  EXPECT_EQ(G_T[2][0].thread_id, 0u);
  EXPECT_EQ(G_T[2][1].thread_id, 1u);
  ASSERT_EQ(G_T[3].size(), 1u);
  EXPECT_EQ(G_T[3][0].thread_id, 2u);
}

TEST(Monitor, CombinedViewCycle) {
  // The butterfly trace from data/cycles.txt: neither input view is cyclic.
  Monitor p(0), q(1), observer(2);
  Monitor l1(0), l2(0);
  p.BeginTransaction();
  q.BeginTransaction();
  p.Release(l1);
  q.Release(l2);
  ASSERT_EQ(q.Acquire(l1), AcquireResult::Ok);
  q.Release(l1);
  ASSERT_EQ(p.Acquire(l2), AcquireResult::Ok);
  p.Release(l2);
  observer.BeginTransaction();
  ASSERT_EQ(observer.Acquire(l1), AcquireResult::Ok);
  EXPECT_EQ(observer.Acquire(l2), AcquireResult::CombinedViewCycle);
}

TEST(Monitor, BackpropagationAcrossAlternatingViewsFindsLaterCycle) {
  // CYCLE_ALTERNATING_PATH_LATER_VIOLATION from data/cycles.txt.
  Monitor p(0), r1(1), r2(2), r3(3), t(4);
  Monitor l1(0), l2(0), l3(0), l4(0), l5(0), l6(0);
  p.BeginTransaction();
  r1.BeginTransaction();
  r2.BeginTransaction();
  r3.BeginTransaction();
  t.BeginTransaction();
  p.Release(l6);
  r3.Release(l4);
  r1.Release(l2);
  r1.Release(l3);
  r2.Release(l1);
  t.Release(l5);

  ASSERT_EQ(t.Acquire(l6), AcquireResult::Ok);
  ASSERT_EQ(p.Acquire(l4), AcquireResult::Ok);
  p.EndTransaction();
  p.BeginTransaction();
  t.EndTransaction();
  t.BeginTransaction();
  ASSERT_EQ(t.Acquire(l2), AcquireResult::Ok);
  t.Release(l6);
  ASSERT_EQ(p.Acquire(l6), AcquireResult::Ok);
  ASSERT_EQ(r2.Acquire(l3), AcquireResult::Ok);
  r2.Release(l3);
  ASSERT_EQ(p.Acquire(l3), AcquireResult::Ok);
  ASSERT_EQ(r3.Acquire(l1), AcquireResult::Ok);
  t.EndTransaction();
  t.BeginTransaction();
  r3.Release(l1);
  ASSERT_EQ(t.Acquire(l1), AcquireResult::Ok);
  t.Release(l1);

  // The alternating path must carry t[1] back through both view partitions.
  ASSERT_EQ(p.Acquire(l1), AcquireResult::Ok);
  ASSERT_EQ(r1.Acquire(l5), AcquireResult::Ok);
  r1.Release(l5);
  EXPECT_EQ(p.Acquire(l5), AcquireResult::CombinedViewCycle);
}

TEST(Monitor, AcyclicDiamondJoin) {
  Monitor ancestor(0), left(1), right(2), join(3);
  Monitor l1(0), l2(0), transfer(0);
  ancestor.BeginTransaction();
  left.BeginTransaction();
  right.BeginTransaction();
  join.BeginTransaction();
  ancestor.Release(l1);
  ancestor.Release(l2);
  ASSERT_EQ(left.Acquire(l1), AcquireResult::Ok);
  ASSERT_EQ(right.Acquire(l2), AcquireResult::Ok);
  left.Release(transfer);
  ASSERT_EQ(join.Acquire(transfer), AcquireResult::Ok);
  right.Release(transfer);
  EXPECT_EQ(join.Acquire(transfer), AcquireResult::Ok);
}

TEST(TxnVectorClock, MergeMinPreservesMissingEntriesAsInfinity) {
  TxnVectorClock destination(2), source(101);
  destination.Fill(TXN_ID_MAX);
  source.Fill(TXN_ID_MAX);
  destination.Set(0, 2);
  source.Set(0, 5);
  source.Set(100, 3);
  destination.MergeMin(source);
  EXPECT_EQ(destination.Get(0), 2u);
  EXPECT_EQ(destination.Get(50), TXN_ID_MAX);
  EXPECT_EQ(destination.Get(100), 3u);

  TxnVectorClock shorter(1);
  shorter.Set(0, 1);
  destination.MergeMin(shorter);
  EXPECT_EQ(destination.Get(0), 1u);
  EXPECT_EQ(destination.Get(100), 3u);
  destination.MergeMin(destination);
  EXPECT_EQ(destination.Get(100), 3u);
}

TEST(TxnVectorClock, GrowthPreservesInfinityForFirstRows) {
  TxnVectorClock row(2);
  row.Fill(TXN_ID_MAX);
  row.Set(0, 3);
  row.EnsureThread(100, TXN_ID_MAX);
  EXPECT_EQ(row.Get(0), 3u);
  for (Tid i = 1; i < row.Size(); ++i)
    EXPECT_EQ(row.Get(i), TXN_ID_MAX);
}

TEST(TxnVectorClock, InitiallyZero) {
  // Construct a clock and check that its entries are zero.
  u32 n = 10;
  TxnVectorClock clock{n};
  ASSERT_EQ(clock.Size(), n);
  for (size_t i = 0; i < n; ++i) {
    EXPECT_EQ(clock.Get(i), 0u) << "thread_id = " << i;
  }
}

TEST(TxnVectorClock, GrowthOnEnsureThread) {
  u32 n = 10;
  TxnVectorClock clock{n};
  ASSERT_EQ(clock.Size(), n);
  clock.EnsureThread(n * 2);
  EXPECT_EQ(clock.Size(), n * 4);
  clock.EnsureThread(clock.Size());
  EXPECT_EQ(clock.Size(), n * 8);
}

TEST(OwnedTxnClocks, NewClockInitiallyZero) {
  OwnedTxnClocks clocks;
  clocks.EnsureThread(0);
  TxnVectorClock& clock = clocks.Get(0);
  ASSERT_EQ(clock.Size(), DEFAULT_TXN_CLOCK_SIZE);
  for (Tid i = 0; i < DEFAULT_TXN_CLOCK_SIZE; ++i)
    EXPECT_EQ(clock.Get(i), 0u);
}

TEST(OwnedTxnClocks, RepeatedLookupPreservesClock) {
  OwnedTxnClocks clocks;
  clocks.EnsureThread(2);
  TxnVectorClock& clock = clocks.Get(2);
  clock.Set(1, 7);

  TxnVectorClock& again = clocks.Get(2);
  EXPECT_EQ(&again, &clock);
  EXPECT_EQ(again.Size(), DEFAULT_TXN_CLOCK_SIZE);
  EXPECT_EQ(again.Get(1), 7u);
}

TEST(OwnedTxnClocks, GrowthPreservesClocksAndFillsGaps) {
  OwnedTxnClocks clocks;
  clocks.EnsureThread(0);
  TxnVectorClock& first = clocks.Get(0);
  first.Set(1, 7);

  clocks.EnsureThread(100);
  TxnVectorClock& later = clocks.Get(100);
  later.Set(0, 9);
  clocks.EnsureThread(50);
  TxnVectorClock& gap = clocks.Get(50);
  ASSERT_EQ(gap.Size(), DEFAULT_TXN_CLOCK_SIZE);
  for (Tid i = 0; i < DEFAULT_TXN_CLOCK_SIZE; ++i)
    EXPECT_EQ(gap.Get(i), 0u);

  EXPECT_EQ(&clocks.Get(0), &first);
  EXPECT_EQ(first.Get(1), 7u);
  EXPECT_EQ(&clocks.Get(100), &later);
  EXPECT_EQ(later.Get(0), 9u);
}

TEST(OwnedTxnClocks, EnsureThreadCreatesAndPreservesClock) {
  OwnedTxnClocks clocks;
  clocks.EnsureThread(0);
  TxnVectorClock& existing = clocks.Get(0);
  existing.Set(1, 3);

  clocks.EnsureThread(100);
  TxnVectorClock& clock = clocks.Get(100);
  ASSERT_EQ(clock.Size(), DEFAULT_TXN_CLOCK_SIZE);
  for (Tid i = 0; i < DEFAULT_TXN_CLOCK_SIZE; ++i)
    EXPECT_EQ(clock.Get(i), 0u);

  clock.Set(1, 7);
  clock.EnsureThread(20);
  const uptr grown_size = clock.Size();
  clocks.EnsureThread(100);
  EXPECT_EQ(&clocks.Get(100), &clock);
  EXPECT_EQ(clock.Size(), grown_size);
  EXPECT_EQ(clock.Get(1), 7u);
  EXPECT_EQ(existing.Size(), DEFAULT_TXN_CLOCK_SIZE);
  EXPECT_EQ(existing.Get(1), 3u);
}

TEST(TxnVectorClock, AssignmentCopiesAndResizes) {
  TxnVectorClock source{3};
  source.Set(2, 7);
  TxnVectorClock destination{1};
  destination = source;
  ASSERT_EQ(destination.Size(), 3u);
  EXPECT_EQ(destination.Get(2), 7u);
  source.Set(2, 9);
  EXPECT_EQ(destination.Get(2), 7u);

  TxnVectorClock smaller{1};
  destination = smaller;
  ASSERT_EQ(destination.Size(), 1u);
  EXPECT_EQ(destination.Get(0), 0u);
  destination.EnsureThread(2);
  EXPECT_EQ(destination.Get(2), 0u);
}

TEST(OwnedTxnClocks, AssignmentCopiesIndependentClocks) {
  OwnedTxnClocks destination;
  destination.EnsureThread(0);
  destination.Get(0).EnsureThread(100);
  destination.Get(0).Set(100, 42);
  {
    OwnedTxnClocks source;
    source.EnsureThread(0);
    source.Get(0).Set(1, 7);
    source.EnsureThread(100);
    source.Get(100).EnsureThread(50);
    source.Get(100).Set(50, 9);

    EXPECT_EQ(&(destination = source), &destination);
    EXPECT_NE(&destination.Get(0), &source.Get(0));
    EXPECT_NE(&destination.Get(100), &source.Get(100));
    EXPECT_EQ(destination.Get(0).Size(), source.Get(0).Size());
    EXPECT_EQ(destination.Get(100).Size(), source.Get(100).Size());
    EXPECT_EQ(destination.Get(0).Get(1), 7u);
    EXPECT_EQ(destination.Get(100).Get(50), 9u);

    source.Get(0).Set(1, 8);
    destination.Get(100).Set(50, 10);
    EXPECT_EQ(destination.Get(0).Get(1), 7u);
    EXPECT_EQ(source.Get(100).Get(50), 9u);
  }
  EXPECT_EQ(destination.Get(0).Get(1), 7u);
  EXPECT_EQ(destination.Get(100).Get(50), 10u);
}

TEST(OwnedTxnClocks, AssignmentRemovesAbsentClocks) {
  OwnedTxnClocks source;
  source.EnsureThread(0);
  OwnedTxnClocks destination;
  destination.EnsureThread(2);
  destination.Get(2).Set(1, 7);
  destination.EnsureThread(100);
  destination.Get(100).Set(1, 9);

  destination = source;
  destination.EnsureThread(2);
  destination.EnsureThread(100);
  EXPECT_EQ(destination.Get(2).Get(1), 0u);
  EXPECT_EQ(destination.Get(100).Get(1), 0u);

  OwnedTxnClocks empty;
  destination.Get(0).Set(1, 42);
  destination = empty;
  destination.EnsureThread(0);
  EXPECT_EQ(destination.Get(0).Get(1), 0u);
}

TEST(OwnedTxnClocks, SelfAssignmentPreservesClocks) {
  OwnedTxnClocks clocks;
  clocks.EnsureThread(100);
  TxnVectorClock& clock = clocks.Get(100);
  clock.Set(1, 7);
  const OwnedTxnClocks& alias = clocks;
  clocks = alias;
  EXPECT_EQ(&clocks.Get(100), &clock);
  EXPECT_EQ(clock.Get(1), 7u);
}

}  // namespace __tsan
