#include "queues.hpp"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void updateMaxDepth(std::atomic<std::uint64_t>& max_depth, std::uint64_t depth) {
  auto current = max_depth.load(std::memory_order_relaxed);
  while (depth > current &&
         !max_depth.compare_exchange_weak(current, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

WaitStrategy resolveWaitStrategy(QueueKind kind, WaitStrategy requested) {
  if (requested != WaitStrategy::QueueDefault) return requested;

  switch (kind) {
    case QueueKind::GlobalLock:
      return WaitStrategy::ConditionVariable;
    case QueueKind::SplitLockLinked:
    case QueueKind::MpmcRing:
    case QueueKind::SpscRing:
      return WaitStrategy::SpinYield;
  }
  return WaitStrategy::QueueDefault;
}

class SpinYieldWaiter {
 public:
  explicit SpinYieldWaiter(QueueStats& stats) : stats_(stats) {}

  void pause() {
    if (spins_ < kSpinBeforeYield) {
      ++spins_;
      stats_.spin_count.fetch_add(1, std::memory_order_relaxed);
      std::atomic_signal_fence(std::memory_order_seq_cst);
      return;
    }
    stats_.yield_count.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::yield();
  }

 private:
  static constexpr std::size_t kSpinBeforeYield = 64;
  std::size_t spins_ = 0;
  QueueStats& stats_;
};

struct alignas(64) PaddedIndex {
  std::atomic<std::size_t> value{0};
};

class GlobalLockQueue final : public IEventQueue {
 public:
  GlobalLockQueue(std::size_t capacity, QueueOptions options)
      : capacity_(capacity),
        wait_strategy_(resolveWaitStrategy(QueueKind::GlobalLock, options.wait_strategy)) {}

  GlobalLockQueue(const GlobalLockQueue&) = delete;
  GlobalLockQueue& operator=(const GlobalLockQueue&) = delete;

  bool push(Event ev, BackpressurePolicy policy, const std::atomic<bool>& stop_flag) override {
    stats_.push_attempts.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(mu_);
    if (closed_) {
      stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    if (capacity_ != 0) {
      if (policy == BackpressurePolicy::Block) {
        if (wait_strategy_ == WaitStrategy::ConditionVariable) {
          stats_.park_count.fetch_add(1, std::memory_order_relaxed);
          not_full_cv_.wait(lock, [&] {
            return closed_ || stop_flag.load(std::memory_order_relaxed) || queue_.size() < capacity_;
          });
          if (closed_ || stop_flag.load(std::memory_order_relaxed)) {
            stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
            return false;
          }
        } else {
          SpinYieldWaiter waiter(stats_);
          while (queue_.size() >= capacity_) {
            stats_.full_hits.fetch_add(1, std::memory_order_relaxed);
            if (closed_ || stop_flag.load(std::memory_order_relaxed)) {
              stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
              return false;
            }
            lock.unlock();
            waiter.pause();
            lock.lock();
          }
        }
      } else {
        if (queue_.size() >= capacity_) {
          stats_.full_hits.fetch_add(1, std::memory_order_relaxed);
          stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
      }
    }

    queue_.push_back(std::move(ev));
    updateMaxDepth(stats_.max_observed_depth, static_cast<std::uint64_t>(queue_.size()));
    stats_.push_success.fetch_add(1, std::memory_order_relaxed);
    not_empty_cv_.notify_one();
    return true;
  }

  bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
    std::unique_lock<std::mutex> lock(mu_);
    if (wait_strategy_ == WaitStrategy::ConditionVariable) {
      stats_.park_count.fetch_add(1, std::memory_order_relaxed);
      not_empty_cv_.wait(lock, [&] {
        return closed_ || stop_flag.load(std::memory_order_relaxed) || !queue_.empty();
      });
    } else {
      SpinYieldWaiter waiter(stats_);
      while (queue_.empty()) {
        stats_.empty_hits.fetch_add(1, std::memory_order_relaxed);
        if (closed_ || stop_flag.load(std::memory_order_relaxed)) return false;
        lock.unlock();
        waiter.pause();
        lock.lock();
      }
    }
    if (queue_.empty()) return false;

    out = std::move(queue_.front());
    queue_.pop_front();
    stats_.pop_success.fetch_add(1, std::memory_order_relaxed);
    not_full_cv_.notify_one();
    return true;
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    not_empty_cv_.notify_all();
    not_full_cv_.notify_all();
  }

  const QueueStats& stats() const noexcept override { return stats_; }

 private:
  std::mutex mu_;
  std::condition_variable not_empty_cv_;
  std::condition_variable not_full_cv_;
  std::deque<Event> queue_;
  std::size_t capacity_ = 0;
  bool closed_ = false;
  WaitStrategy wait_strategy_ = WaitStrategy::ConditionVariable;
  QueueStats stats_{};
};

class SplitLockLinkedQueue final : public IEventQueue {
 public:
  using Semaphore = std::counting_semaphore<>;

  SplitLockLinkedQueue(std::size_t capacity, QueueOptions options)
      : capacity_(capacity),
        wait_strategy_(resolveWaitStrategy(QueueKind::SplitLockLinked, options.wait_strategy)),
        head_(new Node()),
        tail_(head_),
        items_(0) {
    if (capacity_ != 0) {
      slots_ = std::make_unique<Semaphore>(static_cast<std::ptrdiff_t>(capacity_));
    }
  }

  SplitLockLinkedQueue(const SplitLockLinkedQueue&) = delete;
  SplitLockLinkedQueue& operator=(const SplitLockLinkedQueue&) = delete;

  ~SplitLockLinkedQueue() override { destroyNodes(); }

  bool push(Event ev, BackpressurePolicy policy, const std::atomic<bool>& stop_flag) override {
    stats_.push_attempts.fetch_add(1, std::memory_order_relaxed);
    if (closed_.load(std::memory_order_acquire)) {
      stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    SpinYieldWaiter waiter(stats_);
    bool reserved_slot = false;
    if (slots_) {
      if (policy == BackpressurePolicy::Block) {
        while (!slots_->try_acquire()) {
          stats_.full_hits.fetch_add(1, std::memory_order_relaxed);
          if (closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed)) {
            stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
            return false;
          }
          waiter.pause();
        }
        reserved_slot = true;
      } else {
        if (!slots_->try_acquire()) {
          stats_.full_hits.fetch_add(1, std::memory_order_relaxed);
          stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        reserved_slot = true;
      }
    }

    Node* node = new Node(std::move(ev));
    {
      std::lock_guard<std::mutex> lock(tail_mu_);
      if (closed_.load(std::memory_order_relaxed)) {
        delete node;
        if (reserved_slot && slots_) slots_->release();
        stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      tail_->next = node;
      tail_ = node;
      const auto depth = depth_.fetch_add(1, std::memory_order_relaxed) + 1;
      updateMaxDepth(stats_.max_observed_depth, depth);
    }

    items_.release();
    stats_.push_success.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
    SpinYieldWaiter waiter(stats_);
    while (!items_.try_acquire()) {
      stats_.empty_hits.fetch_add(1, std::memory_order_relaxed);
      if (closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed)) {
        return false;
      }
      waiter.pause();
    }

    Node* old_head = nullptr;
    Node* new_head = nullptr;
    {
      std::lock_guard<std::mutex> lock(head_mu_);
      old_head = head_;
      new_head = old_head->next;
      if (new_head == nullptr) {
        return !(closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed));
      }
      out = std::move(*(new_head->value));
      head_ = new_head;
      depth_.fetch_sub(1, std::memory_order_relaxed);
    }

    delete old_head;
    if (slots_) slots_->release();
    stats_.pop_success.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void close() override {
    closed_.store(true, std::memory_order_release);
  }

  const QueueStats& stats() const noexcept override { return stats_; }

 private:
  struct Node {
    Node() = default;
    explicit Node(Event ev) : value(std::move(ev)) {}

    std::optional<Event> value;
    Node* next = nullptr;
  };

  void destroyNodes() {
    Node* node = head_;
    while (node != nullptr) {
      Node* next = node->next;
      delete node;
      node = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
  }

  const std::size_t capacity_;
  [[maybe_unused]] WaitStrategy wait_strategy_ = WaitStrategy::SpinYield;
  std::mutex head_mu_;
  std::mutex tail_mu_;
  Node* head_ = nullptr;
  Node* tail_ = nullptr;
  std::atomic<bool> closed_{false};
  std::atomic<std::uint64_t> depth_{0};
  Semaphore items_;
  std::unique_ptr<Semaphore> slots_;
  QueueStats stats_{};
};

class MpmcRingQueue final : public IEventQueue {
 public:
  explicit MpmcRingQueue(std::size_t capacity, QueueOptions options)
      : ring_capacity_(capacity),
        wait_strategy_(resolveWaitStrategy(QueueKind::MpmcRing, options.wait_strategy)),
        buffer_(ring_capacity_),
        items_(0),
        slots_available_(static_cast<std::ptrdiff_t>(capacity)) {
    if (capacity == 0) {
      throw std::runtime_error("mpmc_ring은 capacity > 0이 필요합니다.");
    }
  }

  bool push(Event ev, BackpressurePolicy policy, const std::atomic<bool>& stop_flag) override {
    stats_.push_attempts.fetch_add(1, std::memory_order_relaxed);
    SpinYieldWaiter waiter(stats_);

    if (policy == BackpressurePolicy::Block) {
      while (!slots_available_.try_acquire()) {
        stats_.full_hits.fetch_add(1, std::memory_order_relaxed);
        if (closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed)) {
          stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        waiter.pause();
      }
    } else {
      if (!slots_available_.try_acquire()) {
        stats_.full_hits.fetch_add(1, std::memory_order_relaxed);
        stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    }

    if (closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed)) {
      slots_available_.release();
      stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(tail_mu_);
      buffer_[tail_] = std::move(ev);
      tail_ = nextIndex(tail_);
      const auto depth = depth_.fetch_add(1, std::memory_order_relaxed) + 1;
      updateMaxDepth(stats_.max_observed_depth, depth);
    }
    items_.release();
    stats_.push_success.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
    SpinYieldWaiter waiter(stats_);

    while (!items_.try_acquire()) {
      stats_.empty_hits.fetch_add(1, std::memory_order_relaxed);
      if (closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed)) {
        return false;
      }
      waiter.pause();
    }

    {
      std::lock_guard<std::mutex> lock(head_mu_);
      out = std::move(buffer_[head_]);
      head_ = nextIndex(head_);
      depth_.fetch_sub(1, std::memory_order_relaxed);
    }
    slots_available_.release();
    stats_.pop_success.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void close() override {
    closed_.store(true, std::memory_order_release);
  }

  const QueueStats& stats() const noexcept override { return stats_; }

 private:
  std::size_t nextIndex(std::size_t idx) const noexcept {
    ++idx;
    return idx == ring_capacity_ ? 0 : idx;
  }

  const std::size_t ring_capacity_;
  [[maybe_unused]] WaitStrategy wait_strategy_ = WaitStrategy::SpinYield;
  std::vector<Event> buffer_;
  std::mutex head_mu_;
  std::mutex tail_mu_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::atomic<std::uint64_t> depth_{0};
  std::atomic<bool> closed_{false};
  std::counting_semaphore<> items_;
  std::counting_semaphore<> slots_available_;
  QueueStats stats_{};
};

class SpscRingQueue final : public IEventQueue {
 public:
  SpscRingQueue(std::size_t capacity, QueueOptions options)
      : ring_capacity_(capacity + 1),
        buffer_(ring_capacity_),
        wait_strategy_(resolveWaitStrategy(QueueKind::SpscRing, options.wait_strategy)) {
    if (capacity == 0) {
      throw std::runtime_error("spsc는 capacity > 0이 필요합니다.");
    }
  }

  bool push(Event ev, BackpressurePolicy policy, const std::atomic<bool>& stop_flag) override {
    stats_.push_attempts.fetch_add(1, std::memory_order_relaxed);
    SpinYieldWaiter waiter(stats_);

    while (true) {
      if (closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed)) {
        stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
      }

      const auto tail = tail_.value.load(std::memory_order_relaxed);
      const auto next = nextIndex(tail);
      const auto head = head_.value.load(std::memory_order_acquire);
      if (next != head) {
        buffer_[tail] = std::move(ev);
        tail_.value.store(next, std::memory_order_release);
        const auto depth = (next > head) ? (next - head) : (ring_capacity_ - head + next);
        updateMaxDepth(stats_.max_observed_depth, static_cast<std::uint64_t>(depth));
        stats_.push_success.fetch_add(1, std::memory_order_relaxed);
        return true;
      }

      stats_.full_hits.fetch_add(1, std::memory_order_relaxed);
      if (policy == BackpressurePolicy::Drop) {
        stats_.push_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      waiter.pause();
    }
  }

  bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
    SpinYieldWaiter waiter(stats_);

    while (true) {
      const auto head = head_.value.load(std::memory_order_relaxed);
      const auto tail = tail_.value.load(std::memory_order_acquire);
      if (head != tail) {
        out = std::move(buffer_[head]);
        head_.value.store(nextIndex(head), std::memory_order_release);
        stats_.pop_success.fetch_add(1, std::memory_order_relaxed);
        return true;
      }

      stats_.empty_hits.fetch_add(1, std::memory_order_relaxed);
      if (closed_.load(std::memory_order_acquire) || stop_flag.load(std::memory_order_relaxed)) {
        return false;
      }
      waiter.pause();
    }
  }

  void close() override {
    closed_.store(true, std::memory_order_release);
  }

  const QueueStats& stats() const noexcept override { return stats_; }

 private:
  std::size_t nextIndex(std::size_t idx) const noexcept {
    ++idx;
    return idx == ring_capacity_ ? 0 : idx;
  }

  const std::size_t ring_capacity_;
  std::vector<Event> buffer_;
  PaddedIndex head_{};
  PaddedIndex tail_{};
  std::atomic<bool> closed_{false};
  [[maybe_unused]] WaitStrategy wait_strategy_ = WaitStrategy::SpinYield;
  QueueStats stats_{};
};

} // namespace

WaitStrategy parseWaitStrategy(std::string_view name) {
  if (name == "default" || name == "queue_default" || name == "queue-default") {
    return WaitStrategy::QueueDefault;
  }
  if (name == "cv" || name == "condition_variable" || name == "condition-variable") {
    return WaitStrategy::ConditionVariable;
  }
  if (name == "spin" || name == "spin_yield" || name == "spin-yield") {
    return WaitStrategy::SpinYield;
  }
  throw std::runtime_error("wait_strategy는 현재 'default', 'cv', 'spin_yield'만 지원합니다.");
}

QueueKind parseQueueKind(std::string_view name) {
  if (name == "global_lock" || name == "global-lock" || name == "mutex") {
    return QueueKind::GlobalLock;
  }
  if (name == "split_lock" || name == "split-lock" || name == "split_linked" ||
      name == "split-linked" || name == "twolock" || name == "two-lock") {
    return QueueKind::SplitLockLinked;
  }
  if (name == "mpmc_ring" || name == "mpmc-ring" || name == "mpmc") {
    return QueueKind::MpmcRing;
  }
  if (name == "spsc") {
    return QueueKind::SpscRing;
  }
  throw std::runtime_error("queue_type은 현재 'global_lock', 'split_lock', 'mpmc_ring', 'spsc'만 지원합니다.");
}

std::string_view queueKindName(QueueKind kind) noexcept {
  switch (kind) {
    case QueueKind::GlobalLock:
      return "global_lock";
    case QueueKind::SplitLockLinked:
      return "split_lock";
    case QueueKind::MpmcRing:
      return "mpmc_ring";
    case QueueKind::SpscRing:
      return "spsc";
  }
  return "unknown";
}

std::string_view waitStrategyName(WaitStrategy strategy) noexcept {
  switch (strategy) {
    case WaitStrategy::QueueDefault:
      return "default";
    case WaitStrategy::ConditionVariable:
      return "cv";
    case WaitStrategy::SpinYield:
      return "spin_yield";
  }
  return "unknown";
}

QueueStatsSnapshot snapshotQueueStats(const QueueStats& stats) noexcept {
  QueueStatsSnapshot snapshot{};
  snapshot.push_attempts = stats.push_attempts.load(std::memory_order_relaxed);
  snapshot.push_success = stats.push_success.load(std::memory_order_relaxed);
  snapshot.push_fail = stats.push_fail.load(std::memory_order_relaxed);
  snapshot.pop_success = stats.pop_success.load(std::memory_order_relaxed);
  snapshot.spin_count = stats.spin_count.load(std::memory_order_relaxed);
  snapshot.yield_count = stats.yield_count.load(std::memory_order_relaxed);
  snapshot.park_count = stats.park_count.load(std::memory_order_relaxed);
  snapshot.full_hits = stats.full_hits.load(std::memory_order_relaxed);
  snapshot.empty_hits = stats.empty_hits.load(std::memory_order_relaxed);
  snapshot.max_observed_depth = stats.max_observed_depth.load(std::memory_order_relaxed);
  return snapshot;
}

std::unique_ptr<IEventQueue> makeQueue(QueueKind kind, std::size_t capacity, QueueOptions options) {
  switch (kind) {
    case QueueKind::GlobalLock:
      return std::make_unique<GlobalLockQueue>(capacity, options);
    case QueueKind::SplitLockLinked:
      return std::make_unique<SplitLockLinkedQueue>(capacity, options);
    case QueueKind::MpmcRing:
      return std::make_unique<MpmcRingQueue>(capacity, options);
    case QueueKind::SpscRing:
      return std::make_unique<SpscRingQueue>(capacity, options);
  }
  throw std::runtime_error("알 수 없는 QueueKind입니다.");
}
