#include "tsan_monitor.h"

#include "sanitizer_common/sanitizer_placement_new.h"
#include "tsan_mman.h"
#include "tsan_rtl.h"

namespace __tsan {

TxnVectorClock::TxnVectorClock(uptr size) : vector_() { vector_.Reset(size); }

TxnVectorClock& TxnVectorClock::operator=(const TxnVectorClock& other) {
  vector_ = other.vector_;
  return *this;
}

void TxnVectorClock::EnsureThread(Tid thread_id, TxnId initial_value) {
  const uptr old_size = vector_.Size();
  vector_.EnsureIndex(thread_id);
  for (uptr i = old_size; i < vector_.Size(); ++i) vector_[i] = initial_value;
}

uptr TxnVectorClock::Size() const { return vector_.Size(); }

TxnId TxnVectorClock::Get(Tid thread_id) const {
  DCHECK_LT(thread_id, vector_.Size());
  return vector_[thread_id];
}

void TxnVectorClock::Set(Tid thread_id, TxnId txn_id) {
  DCHECK_LT(thread_id, vector_.Size());
  vector_[thread_id] = txn_id;
}

void TxnVectorClock::Fill(TxnId v) {
  for (u32 i = 0; i < vector_.Size(); ++i) {
    Set(i, v);
  }
}

void TxnVectorClock::MergeMin(const TxnVectorClock& other) {
  if (other.Size())
    EnsureThread(other.Size() - 1, TXN_ID_MAX);
  for (Tid r = 0; r < other.Size(); ++r) {
    if (other.Get(r) < Get(r))
      Set(r, other.Get(r));
  }
}

OwnedTxnClocks::OwnedTxnClocks() : clocks_() {}

OwnedTxnClocks::~OwnedTxnClocks() {
  for (uptr i = 0; i < clocks_.Size(); ++i) DestroyTxnClock(clocks_[i]);
}

void OwnedTxnClocks::EnsureThread(Tid thread_id, TxnId initial_value) {
  clocks_.EnsureIndex(thread_id);
  if (!clocks_[thread_id]) {
    clocks_[thread_id] = AllocateTxnClock(DEFAULT_TXN_CLOCK_SIZE);
    clocks_[thread_id]->Fill(initial_value);
  }
}

TxnVectorClock& OwnedTxnClocks::Get(Tid thread_id) {
  DCHECK_LT(thread_id, clocks_.Size());
  DCHECK(clocks_[thread_id] != nullptr);
  return *clocks_[thread_id];
}

// NOTE: monitor will hanlde its own locking

Monitor::Monitor(Tid tid)
    : thread_id_(tid),
      last_used_by_(kInvalidTid),
      active_(),
      last_(DEFAULT_TXN_CLOCK_SIZE) {
  last_.EnsureThread(thread_id_);
  active_.EnsureIndex(thread_id_);
  first_.EnsureThread(thread_id_, TXN_ID_MAX);
}

void Monitor::BeginTransaction() {
  TxnId curr_txn = last_.Get(thread_id_);
  last_.Set(thread_id_, curr_txn + 1);
  active_[thread_id_] = true;
  auto& clock = first_.Get(thread_id_);
  // set to "infinity"
  clock.Fill(TXN_ID_MAX);
  clock.EnsureThread(thread_id_, TXN_ID_MAX);
  clock.Set(thread_id_, last_.Get(thread_id_));
}

void Monitor::EndTransaction() { active_[thread_id_] = false; }

void Monitor::Release(Monitor& l) {
  // copy
  l.last_ = last_;
  l.first_ = first_;
  l.active_ = active_;
  l.last_used_by_ = thread_id_;
}

Monitor* Monitor::GetView() {
  // this gets a snapshot of the view of the monitor
  Monitor* write_monitor = New<Monitor>(thread_id_);
  Release(*write_monitor);
  return write_monitor;
}

ConflictGraph::ConflictGraph(Monitor& p, Monitor& l) {
  for (Tid r = 0; r < p.active_.Size(); ++r) {
    if (p.active_[r]) {
      nodes.PushBack({View::Thread, r});
    }
  }
  const uptr l_begin = nodes.Size();
  for (Tid r = 0; r < l.active_.Size(); ++r) {
    if (l.active_[r]) {
      nodes.PushBack({View::Lock, r});
    }
  }

  for (uptr i = 0; i < l_begin; ++i) {
    const Tid p_thread = nodes[i].thread_id;
    auto& first_p = p.first_.Get(p_thread);
    for (uptr j = l_begin; j < nodes.Size(); ++j) {
      const Tid l_thread = nodes[j].thread_id;
      if (p_thread == l_thread) {
        continue;
      }
      if (l_thread < first_p.Size() &&
          first_p.Get(l_thread) <= l.last_.Get(l_thread)) {
        edges.PushBack({i, j});
      }
      auto& first_l = l.first_.Get(l_thread);

      if (p_thread >= first_l.Size()) {
        continue;
      }

      if (first_l.Get(p_thread) <= p.last_.Get(p_thread)) {
        edges.PushBack({j, i});
      }
    }
  }
}

ConflictGraph::Transposed::Transposed(const ConflictGraph& G) {
  // Group predecessor vertices into a contiguous range for each vertex.
  offsets_.Resize(G.nodes.Size() + 1);
  for (uptr i = 0; i < G.edges.Size(); ++i) ++offsets_[G.edges[i].target + 1];
  for (uptr v = 0; v < G.nodes.Size(); ++v) offsets_[v + 1] += offsets_[v];

  Vector<uptr> cursor;
  cursor.Resize(G.nodes.Size());
  for (uptr v = 0; v < G.nodes.Size(); ++v) cursor[v] = offsets_[v];
  predecessors_.Resize(G.edges.Size());
  for (uptr i = 0; i < G.edges.Size(); ++i) {
    const auto& edge = G.edges[i];
    predecessors_[cursor[edge.target]++] = G.nodes[edge.source];
  }
}

ArrayRef<ConflictGraph::Vertex> ConflictGraph::Transposed::operator[](
    uptr v) const {
  const uptr count = offsets_[v + 1] - offsets_[v];
  if (count == 0)
    return {};
  return {&predecessors_[offsets_[v]], count};
}

bool ConflictGraph::TopSort(Vector<uptr>& order) const {
  order.Resize(0);
  Vector<uptr> indegree;
  Vector<uptr> outgoing;
  Vector<uptr> next;
  indegree.Resize(nodes.Size());
  outgoing.Resize(nodes.Size());
  for (uptr i = 0; i < edges.Size(); ++i) {
    const auto& edge = edges[i];
    ++indegree[edge.target];
    // One-based edge indices leave zero as the end of each adjacency list.
    next.PushBack(outgoing[edge.source]);
    outgoing[edge.source] = i + 1;
  }

  // The order vector also serves as Kahn's queue.
  for (uptr i = 0; i < nodes.Size(); ++i) {
    if (indegree[i] == 0)
      order.PushBack(i);
  }
  for (uptr head = 0; head < order.Size(); ++head) {
    for (uptr link = outgoing[order[head]]; link; link = next[link - 1]) {
      const uptr target = edges[link - 1].target;
      if (--indegree[target] == 0)
        order.PushBack(target);
    }
  }
  return order.Size() == nodes.Size();
}

AcquireResult Monitor::Acquire(Monitor& l) {
  const TxnId incoming_last =
      thread_id_ < l.last_.Size() ? l.last_.Get(thread_id_) : 0;
  if (l.last_used_by_ != kInvalidTid &&
      last_.Get(thread_id_) == incoming_last && l.last_used_by_ != thread_id_) {
    return AcquireResult::IncreasingCycle;
  }
  if (l.last_used_by_ == thread_id_) {
    return AcquireResult::Ok;
  }
  l.last_used_by_ = thread_id_;

  if (l.last_.Size()) {
    last_.EnsureThread(l.last_.Size() - 1);
    active_.EnsureIndex(l.last_.Size() - 1);
  }

  for (u32 r = 0; r < l.last_.Size(); ++r) {
    if (last_.Get(r) >= l.last_.Get(r)) {
      continue;
    }

    // update view of p
    last_.Set(r, l.last_.Get(r));
    active_[r] = l.active_[r];
    first_.EnsureThread(r, TXN_ID_MAX);
    auto& first_p_r = first_.Get(r);
    auto& first_l_r = l.first_.Get(r);
    first_p_r = first_l_r;
    first_p_r.EnsureThread(thread_id_, TXN_ID_MAX);
    first_p_r.Set(thread_id_, last_.Get(thread_id_));
  }

  ConflictGraph G(*this, l);

  Vector<uptr> order;
  if (!G.TopSort(order)) {
    return AcquireResult::CombinedViewCycle;
  }

  auto G_T = G.Transpose();
  Monitor& p_m = *this;
  Monitor& l_m = l;

  // Traverse the topological order backwards, as rev_order in monitor.py.
  for (uptr i = order.Size(); i > 0; --i) {
    const uptr v = order[i - 1];
    const auto& [b, r2] = G.nodes[v];
    for (const auto& [a, r1] : G_T[v]) {
      // Edge (a, r1) --> (b, r2).
      DCHECK(a != b);
      if (a == ConflictGraph::View::Thread) {
        p_m.first_.Get(r1).MergeMin(l_m.first_.Get(r2));
      } else {
        l_m.first_.Get(r1).MergeMin(p_m.first_.Get(r2));
        if (p_m.last_.Get(r1) == l_m.last_.Get(r1))
          p_m.first_.Get(r1).MergeMin(l_m.first_.Get(r1));
      }
    }
  }

  return AcquireResult::Ok;
}

OwnedTxnClocks& OwnedTxnClocks::operator=(const OwnedTxnClocks& other) {
  if (this == &other)
    return *this;

  for (uptr i = 0; i < clocks_.Size(); ++i) DestroyTxnClock(clocks_[i]);
  clocks_.Reset(other.clocks_.Size());

  for (uptr i = 0; i < other.clocks_.Size(); ++i) {
    if (!other.clocks_[i])
      continue;
    clocks_[i] = AllocateTxnClock(other.clocks_[i]->Size());
    *clocks_[i] = *other.clocks_[i];
  }
  return *this;
}

TxnVectorClock* OwnedTxnClocks::AllocateTxnClock(uptr size) {
  return New<TxnVectorClock>(size);
}

void OwnedTxnClocks::DestroyTxnClock(TxnVectorClock*& clock) {
  DestroyAndFree(clock);
}

MemoryState* MonitorRegistry::GetOrCreateMemoryState(uptr addr) {
  Lock guard(&lock);
  if (auto* entry = memory_monitors.find(addr))
    return entry->second;

  auto* state = New<MemoryState>();
  memory_monitors.insert({addr, state});
  return state;
}

void MonitorRead(ThreadState* thr, uptr pc, uptr addr, uptr size) {
  MonitorRegistry& registry = ctx->monitor_registry;
  MemoryState* addr_state = registry.GetOrCreateMemoryState(addr);
  AcquireResult conflict = AcquireResult::Ok;
  {
    Lock guard(&addr_state->lock);

    if (addr_state->writer) {
      // write-read dependency
      conflict = thr->monitor.Acquire(*addr_state->writer);
    };

    Monitor* reader_view = thr->monitor.GetView();
    addr_state->readers.PushBack(reader_view);
  }
  if (conflict != AcquireResult::Ok) {
    ReportTransactionConflict(thr, pc, addr, size, conflict);
  }
}
void MonitorWrite(ThreadState* thr, uptr pc, uptr addr, uptr size) {
  // grapb ctx lock
  MonitorRegistry& registry = ctx->monitor_registry;
  MemoryState* addr_state = registry.GetOrCreateMemoryState(addr);
  AcquireResult conflict = AcquireResult::Ok;
  {
    Lock guard(&addr_state->lock);
    if (addr_state->writer) {
      // write-write dependency
      conflict = thr->monitor.Acquire(*addr_state->writer);
    };
    for (int i = 0; i < addr_state->readers.Size(); ++i) {
      // read-write dependencies
      Monitor* read_monitor = addr_state->readers[i];
      const AcquireResult result = thr->monitor.Acquire(*read_monitor);
      // Keep the first conflict while still processing all dependencies.
      if (conflict == AcquireResult::Ok)
        conflict = result;
    }

    Monitor* writer_view = thr->monitor.GetView();
    addr_state->writer = writer_view;

    // after a write we should clear the readers in MemoryState
    addr_state->readers.Resize(0);
  }
  if (conflict != AcquireResult::Ok) {
    ReportTransactionConflict(thr, pc, addr, size, conflict);
  }
}

void MonitorThreadStart(ThreadState* thr) {
  Monitor& m = thr->monitor;
  // TODO: setup monitor state
}
void MonitorThreadFinish(ThreadState* thr) {
  Monitor& m = thr->monitor;
  // TODO: cleanup monitor
}

}  // namespace __tsan
