// Guest critical sections that hand the lock to the oldest waiter on release.
//
// The SDK's RtlLeaveCriticalSection wakes a waiter by setting the critical
// section's auto-reset event, and its host events keep the signal for
// whichever thread waits next: a thread that leaves and enters again at once
// takes the lock back before the woken waiter runs. The console's kernel
// releases the oldest waiter itself, so the lock goes to it. Fable 2 depends
// on that: its game thread holds a subsystem lock (0x83498D14) while it waits
// for a worker thread (sub_822BA7D0, sub_826194D0), and the worker needs the
// lock to finish; it gets it in the gap between the game thread's frames.
// Without the hand-off the game thread sometimes took the lock back first and
// both threads waited for ever (no input, the 3D engine kept drawing).
//
// The lock count keeps the SDK's (and the kernel's) meaning: -1 free, 0 held,
// each waiter adds one. A contended Enter adds itself and queues in one step
// under its stripe's mutex, and the last Leave takes the oldest waiter off the
// queue under the same mutex, so every waiter counted is queued.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

namespace {

// X_RTL_CRITICAL_SECTION: dispatcher header (byte 1: spin count / 256),
// lock count (host order, as the SDK keeps it), recursion count, owner (KTHREAD).
constexpr uint32_t kSpinCount = 0x01;
constexpr uint32_t kLockCount = 0x10;
constexpr uint32_t kRecursion = 0x14;
constexpr uint32_t kOwner = 0x18;

struct Waiter {
  std::condition_variable cv;
  bool released = false;
};

struct Stripe {
  std::mutex mutex;
  std::unordered_map<uint32_t, std::deque<Waiter *>> queues;
};

Stripe g_stripes[64];

Stripe &StripeFor(uint32_t cs) { return g_stripes[((cs >> 2) ^ (cs >> 9)) & 63]; }

std::atomic_ref<int32_t> LockCount(uint8_t *cs) {
  return std::atomic_ref<int32_t>(*reinterpret_cast<int32_t *>(cs + kLockCount));
}

uint32_t Load(uint8_t *cs, uint32_t offset) { return rex::memory::load_and_swap<uint32_t>(cs + offset); }
void Store(uint8_t *cs, uint32_t offset, uint32_t v) { rex::memory::store_and_swap<uint32_t>(cs + offset, v); }

uint32_t CurrentThread() { return rex::system::XThread::GetCurrentThread()->guest_object(); }

void Own(uint8_t *cs, uint32_t thread) {
  Store(cs, kOwner, thread);
  Store(cs, kRecursion, 1);
}

}  // namespace

extern "C" REX_FUNC(__imp__RtlEnterCriticalSection) {
  (void)base;
  const uint32_t va = ctx.r3.u32;
  uint8_t *cs = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t *>(va);
  const uint32_t thread = CurrentThread();
  auto lock_count = LockCount(cs);
  if (Load(cs, kOwner) == thread) {
    lock_count.fetch_add(1);
    Store(cs, kRecursion, Load(cs, kRecursion) + 1);
    return;
  }
  for (uint32_t spin = uint32_t(cs[kSpinCount]) * 256; spin; --spin) {
    int32_t expected = -1;
    if (lock_count.compare_exchange_strong(expected, 0)) {
      Own(cs, thread);
      return;
    }
  }
  Stripe &stripe = StripeFor(va);
  Waiter waiter;
  {
    std::unique_lock lock(stripe.mutex);
    if (lock_count.fetch_add(1) == -1) {
      lock.unlock();
      Own(cs, thread);
      return;
    }
    stripe.queues[va].push_back(&waiter);
    waiter.cv.wait(lock, [&] { return waiter.released; });
  }
  Own(cs, thread);
}

extern "C" REX_FUNC(__imp__RtlTryEnterCriticalSection) {
  (void)base;
  uint8_t *cs = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t *>(ctx.r3.u32);
  const uint32_t thread = CurrentThread();
  auto lock_count = LockCount(cs);
  int32_t expected = -1;
  if (lock_count.compare_exchange_strong(expected, 0)) {
    Own(cs, thread);
    ctx.r3.u64 = 1;
  } else if (Load(cs, kOwner) == thread) {
    lock_count.fetch_add(1);
    Store(cs, kRecursion, Load(cs, kRecursion) + 1);
    ctx.r3.u64 = 1;
  } else {
    ctx.r3.u64 = 0;
  }
}

extern "C" REX_FUNC(__imp__RtlLeaveCriticalSection) {
  (void)base;
  const uint32_t va = ctx.r3.u32;
  uint8_t *cs = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t *>(va);
  auto lock_count = LockCount(cs);
  const uint32_t recursion = Load(cs, kRecursion) - 1;
  Store(cs, kRecursion, recursion);
  if (recursion != 0) {
    lock_count.fetch_sub(1);
    return;
  }
  Stripe &stripe = StripeFor(va);
  std::lock_guard lock(stripe.mutex);
  Store(cs, kOwner, 0);
  if (lock_count.fetch_sub(1) == 0)
    return;
  auto it = stripe.queues.find(va);
  if (it == stripe.queues.end() || it->second.empty())
    return;  // every waiter counted is queued (both under this mutex)
  Waiter *next = it->second.front();
  it->second.pop_front();
  if (it->second.empty())
    stripe.queues.erase(it);
  next->released = true;
  next->cv.notify_one();
}
