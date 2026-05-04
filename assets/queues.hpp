#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

using EventClock = std::chrono::steady_clock;

enum class BackpressurePolicy {
  Block,
  Drop,
};

enum class WaitStrategy {
  QueueDefault,
  ConditionVariable,
  SpinYield,
};

enum class QueueKind {
  GlobalLock,
  SplitLockLinked,
  MpmcRing,
  SpscRing,
};

struct Event {
  std::uint64_t id = 0;
  std::uint64_t route_key = 0;
  std::size_t target_worker = 0;
  EventClock::time_point created_at{};
};

struct QueueStats {
  std::atomic<std::uint64_t> push_attempts{0};
  std::atomic<std::uint64_t> push_success{0};
  std::atomic<std::uint64_t> push_fail{0};
  std::atomic<std::uint64_t> pop_success{0};
  std::atomic<std::uint64_t> spin_count{0};
  std::atomic<std::uint64_t> yield_count{0};
  std::atomic<std::uint64_t> park_count{0};
  std::atomic<std::uint64_t> full_hits{0};
  std::atomic<std::uint64_t> empty_hits{0};
  std::atomic<std::uint64_t> max_observed_depth{0};
};

struct QueueStatsSnapshot {
  std::uint64_t push_attempts = 0;
  std::uint64_t push_success = 0;
  std::uint64_t push_fail = 0;
  std::uint64_t pop_success = 0;
  std::uint64_t spin_count = 0;
  std::uint64_t yield_count = 0;
  std::uint64_t park_count = 0;
  std::uint64_t full_hits = 0;
  std::uint64_t empty_hits = 0;
  std::uint64_t max_observed_depth = 0;
};

struct QueueOptions {
  WaitStrategy wait_strategy = WaitStrategy::QueueDefault;
};

class IEventQueue {
 public:
  virtual ~IEventQueue() = default;

  virtual bool push(Event ev, BackpressurePolicy policy, const std::atomic<bool>& stop_flag) = 0;
  virtual bool pop(Event& out, const std::atomic<bool>& stop_flag) = 0;
  virtual void close() = 0;
  virtual const QueueStats& stats() const noexcept = 0;
};

QueueKind parseQueueKind(std::string_view name);
WaitStrategy parseWaitStrategy(std::string_view name);
std::string_view queueKindName(QueueKind kind) noexcept;
std::string_view waitStrategyName(WaitStrategy strategy) noexcept;
QueueStatsSnapshot snapshotQueueStats(const QueueStats& stats) noexcept;
std::unique_ptr<IEventQueue> makeQueue(QueueKind kind, std::size_t capacity, QueueOptions options = {});
