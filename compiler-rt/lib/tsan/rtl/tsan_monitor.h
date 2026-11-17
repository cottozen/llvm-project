#ifndef TSAN_MONITOR_H
#define TSAN_MONITOR_H
#include "sanitizer_common/sanitizer_array_ref.h"
#include "sanitizer_common/sanitizer_dense_map.h"
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_vector.h"
#include "tsan_defs.h"

namespace __tsan {

struct ThreadState;

enum class AcquireResult {
  Ok,
  IncreasingCycle,
  CombinedViewCycle,
};

typedef u32 TxnId;
constexpr u32 TXN_ID_MAX = static_cast<u32>(-1);

const uptr DEFAULT_TXN_CLOCK_SIZE = 10;

template <typename T>
class ResizingVector {
 public:
  ResizingVector() { vector_.Resize(DEFAULT_TXN_CLOCK_SIZE); }

  void Reset(uptr size) {
    vector_.Reset();
    vector_.Resize(size);
  }

  ResizingVector& operator=(const ResizingVector& other) {
    if (this == &other)
      return *this;
    vector_.Resize(other.Size());
    for (uptr i = 0; i < other.Size(); ++i) vector_[i] = other[i];
    return *this;
  }

  void EnsureIndex(uptr index) {
    if (index < vector_.Size())
      return;

    uptr size = vector_.Size();
    if (size == 0)
      size = 1;

    while (size <= index) size *= 2;

    vector_.Resize(size);
  }

  uptr Size() const { return vector_.Size(); }

  T& operator[](uptr index) { return vector_[index]; }

  const T& operator[](uptr index) const { return vector_[index]; }

 private:
  Vector<T> vector_;
};

class TxnVectorClock {
 public:
  TxnVectorClock(uptr size);

  TxnVectorClock& operator=(const TxnVectorClock& other);

  void EnsureThread(Tid thread_id, TxnId initial_value = 0);

  uptr Size() const;

  TxnId Get(Tid thread_id) const;

  void Set(Tid thread_id, TxnId txn_id);

  void Fill(TxnId v);

  // Componentwise minimum, treating missing entries as infinity.
  void MergeMin(const TxnVectorClock& other);

 private:
  ResizingVector<TxnId> vector_;
};

// Owns separately allocated clocks so vector growth only relocates pointers.
class OwnedTxnClocks {
 public:
  OwnedTxnClocks();

  ~OwnedTxnClocks();

  OwnedTxnClocks(const OwnedTxnClocks&) = delete;
  OwnedTxnClocks& operator=(const OwnedTxnClocks& other);

  // Creates the thread's clock if missing, without growing any inner clocks.
  void EnsureThread(Tid thread_id, TxnId initial_value = 0);

  // Returns an existing clock without allocating. EnsureThread must have been
  // called for this thread.
  TxnVectorClock& Get(Tid thread_id);

 private:
  // Keep runtime allocation details out of headers shared with unit tests.
  static TxnVectorClock* AllocateTxnClock(uptr size);
  static void DestroyTxnClock(TxnVectorClock*& clock);

  ResizingVector<TxnVectorClock*> clocks_;
};

class Monitor;

// Vertices distinguish the two views even when they refer to the same thread.
struct ConflictGraph {
  enum class View { Thread, Lock };
  struct Vertex {
    View view;
    Tid thread_id;
  };
  struct Edge {
    uptr source;
    uptr target;
  };

  class Transposed {
   public:
    explicit Transposed(const ConflictGraph& G);
    ArrayRef<Vertex> operator[](uptr v) const;

   private:
    Vector<uptr> offsets_;
    Vector<Vertex> predecessors_;
  };

  ConflictGraph(Monitor& p, Monitor& l);

  // Returns false if cyclic; otherwise order contains every vertex index.
  bool TopSort(Vector<uptr>& order) const;

  Transposed Transpose() const { return Transposed(*this); }

  Vector<Vertex> nodes;
  Vector<Edge> edges;
};

class Monitor {
 public:
  explicit Monitor(Tid tid);

  void BeginTransaction();
  void EndTransaction();

  void Release(Monitor& l);
  Monitor* GetView();

  AcquireResult Acquire(Monitor& l);

 private:
  friend struct ConflictGraph;

  Tid thread_id_;
  // Thread that published this state; unset until Release/GetView publishes it.
  Tid last_used_by_;
  ResizingVector<bool> active_;
  TxnVectorClock last_;
  OwnedTxnClocks first_;
};

struct MemoryState {
  Monitor* writer = nullptr;
  __sanitizer::Vector<Monitor*> readers;
  Mutex lock;
};

struct MonitorRegistry {
  __sanitizer::DenseMap<uptr, MemoryState*> memory_monitors;
  __sanitizer::DenseMap<uptr, Monitor*> lock_monitors;
  Mutex lock;

  MemoryState* GetOrCreateMemoryState(uptr addr);
};

void MonitorRead(ThreadState* thr, uptr pc, uptr addr, uptr size);
void MonitorWrite(ThreadState* thr, uptr pc, uptr addr, uptr size);

void MonitorThreadStart(ThreadState* thr);
void MonitorThreadFinish(ThreadState* thr);

}  // namespace __tsan

#endif
