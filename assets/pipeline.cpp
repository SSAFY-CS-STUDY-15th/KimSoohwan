#include "pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using SteadyClock = EventClock;

struct WorkerMetrics {
  std::uint64_t completed = 0;
  std::vector<std::int64_t> latencies_ns;
};

struct PipelineState {
  std::atomic<bool> ingress_stop{false};
  std::atomic<std::int64_t> measure_start_ns{-1};
  std::atomic<std::uint64_t> generated{0};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> completed{0};
  std::atomic<std::uint64_t> dropped_ingress{0};
  std::atomic<std::uint64_t> dropped_dispatch{0};
};

void mergeQueueStats(QueueStatsSnapshot& dst, const QueueStatsSnapshot& src) {
  dst.push_attempts += src.push_attempts;
  dst.push_success += src.push_success;
  dst.push_fail += src.push_fail;
  dst.pop_success += src.pop_success;
  dst.spin_count += src.spin_count;
  dst.yield_count += src.yield_count;
  dst.park_count += src.park_count;
  dst.full_hits += src.full_hits;
  dst.empty_hits += src.empty_hits;
  dst.max_observed_depth = std::max(dst.max_observed_depth, src.max_observed_depth);
}

std::uint64_t nextRandom(std::uint64_t& state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

void simulateWork(std::uint64_t service_ns, std::uint64_t jitter_ns, std::uint64_t& rng_state) {
  if (service_ns == 0 && jitter_ns == 0) return;

  std::uint64_t total_ns = service_ns;
  if (jitter_ns > 0) {
    total_ns += nextRandom(rng_state) % (jitter_ns + 1);
  }

  const auto deadline = SteadyClock::now() + std::chrono::nanoseconds(total_ns);
  while (SteadyClock::now() < deadline) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
}

bool isMeasuredEvent(const Event& ev, const PipelineState& state) {
  const auto measure_start_ns = state.measure_start_ns.load(std::memory_order_relaxed);
  const auto created_ns =
    std::chrono::duration_cast<std::chrono::nanoseconds>(ev.created_at.time_since_epoch()).count();
  return measure_start_ns >= 0 && created_ns >= measure_start_ns;
}

bool isMeasuredTimestamp(const SteadyClock::time_point& created_at, const PipelineState& state) {
  const auto measure_start_ns = state.measure_start_ns.load(std::memory_order_relaxed);
  const auto created_ns =
    std::chrono::duration_cast<std::chrono::nanoseconds>(created_at.time_since_epoch()).count();
  return measure_start_ns >= 0 && created_ns >= measure_start_ns;
}

double percentileLatencyUs(std::vector<std::int64_t>& sorted_latency_ns, double p) {
  if (sorted_latency_ns.empty()) return 0.0;
  if (p <= 0.0) return sorted_latency_ns.front() / 1000.0;
  if (p >= 100.0) return sorted_latency_ns.back() / 1000.0;

  const double rank = (p / 100.0) * static_cast<double>(sorted_latency_ns.size() - 1);
  const std::size_t idx = static_cast<std::size_t>(std::llround(rank));
  return sorted_latency_ns[std::min(idx, sorted_latency_ns.size() - 1)] / 1000.0;
}

void validatePipelineConfig(const PipelineConfig& config) {
  if (config.ingress_threads <= 0) throw std::runtime_error("ingress_threads는 1 이상이어야 합니다.");
  if (config.worker_threads <= 0) throw std::runtime_error("worker_threads는 1 이상이어야 합니다.");
  if (config.duration.count() <= 0) throw std::runtime_error("duration은 1ms 이상이어야 합니다.");
  if (config.warmup.count() < 0) throw std::runtime_error("warmup은 0ms 이상이어야 합니다.");
  if (config.rate_per_ingress < 0.0) throw std::runtime_error("rate는 0 이상이어야 합니다.");
  if (config.ingress_queue_capacity == 0) {
    throw std::runtime_error("ingress_queue_capacity는 1 이상이어야 합니다.");
  }

  if (config.topology == PipelineTopology::SharedQueue) {
    if (config.shared_queue_kind == QueueKind::SpscRing) {
      throw std::runtime_error("shared_queue topology에는 spsc를 직접 사용할 수 없습니다.");
    }
    return;
  }

  if (config.dispatch_queue_kind == QueueKind::SpscRing) {
    throw std::runtime_error("local_spsc topology의 dispatch queue는 shared queue여야 합니다.");
  }
  if (config.worker_queue_capacity == 0) {
    throw std::runtime_error("local_spsc topology에는 worker_queue_capacity > 0이 필요합니다.");
  }
  if (config.topology == PipelineTopology::DirectLocalSpsc && config.ingress_threads > config.worker_threads) {
    throw std::runtime_error("direct_local_spsc는 ingress_threads <= worker_threads 조건이 필요합니다.");
  }
}

PipelineResult finalizeResult(const PipelineConfig& config,
                              const PipelineState& state,
                              std::vector<WorkerMetrics>& worker_metrics,
                              const QueueStatsSnapshot& ingress_queue_stats,
                              const QueueStatsSnapshot& worker_queue_stats) {
  PipelineResult result{};
  result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(config.duration);
  result.generated = state.generated.load(std::memory_order_relaxed);
  result.accepted = state.accepted.load(std::memory_order_relaxed);
  result.completed = state.completed.load(std::memory_order_relaxed);
  result.dropped_ingress = state.dropped_ingress.load(std::memory_order_relaxed);
  result.dropped_dispatch = state.dropped_dispatch.load(std::memory_order_relaxed);
  result.dropped = result.dropped_ingress + result.dropped_dispatch;
  result.ingress_queue_stats = ingress_queue_stats;
  result.worker_queue_stats = worker_queue_stats;

  std::vector<std::int64_t> all_latencies;
  std::vector<long double> worker_completed_values;
  worker_completed_values.reserve(worker_metrics.size());
  result.worker_completed_min = std::numeric_limits<std::uint64_t>::max();
  result.worker_completed_max = 0;

  for (auto& metrics : worker_metrics) {
    all_latencies.insert(all_latencies.end(), metrics.latencies_ns.begin(), metrics.latencies_ns.end());
    result.worker_completed_min = std::min(result.worker_completed_min, metrics.completed);
    result.worker_completed_max = std::max(result.worker_completed_max, metrics.completed);
    worker_completed_values.push_back(static_cast<long double>(metrics.completed));
  }

  if (worker_metrics.empty()) {
    result.worker_completed_min = 0;
  }

  result.samples = all_latencies.size();
  if (!all_latencies.empty()) {
    long double sum = 0;
    for (const auto v : all_latencies) sum += static_cast<long double>(v);
    const long double avg_ns = sum / static_cast<long double>(all_latencies.size());
    result.avg_latency_us = static_cast<double>(avg_ns / 1000.0L);

    std::sort(all_latencies.begin(), all_latencies.end());
    result.p95_latency_us = percentileLatencyUs(all_latencies, 95.0);
    result.p99_latency_us = percentileLatencyUs(all_latencies, 99.0);
  }

  if (!worker_completed_values.empty()) {
    long double sum = 0;
    for (const auto v : worker_completed_values) sum += v;
    const long double avg = sum / static_cast<long double>(worker_completed_values.size());
    result.worker_completed_avg = static_cast<double>(avg);

    long double variance = 0;
    for (const auto v : worker_completed_values) {
      const long double delta = v - avg;
      variance += delta * delta;
    }
    variance /= static_cast<long double>(worker_completed_values.size());
    result.worker_completed_stddev = static_cast<double>(std::sqrt(variance));
    if (avg > 0.0L) {
      result.worker_imbalance_pct = static_cast<double>(
        (static_cast<long double>(result.worker_completed_max - result.worker_completed_min) / avg) * 100.0L);
    }
  }

  const double measure_s = std::chrono::duration<double>(config.duration).count();
  result.tps_completed = measure_s > 0.0 ? static_cast<double>(result.completed) / measure_s : 0.0;
  return result;
}

PipelineResult runSharedQueuePipeline(const PipelineConfig& config) {
  PipelineState state;
  std::vector<WorkerMetrics> worker_metrics(static_cast<std::size_t>(config.worker_threads));
  auto shared_queue = makeQueue(
    config.shared_queue_kind, config.ingress_queue_capacity, QueueOptions{.wait_strategy = config.wait_strategy});
  // TODO(phase4): topology-specific wait tuning should allow separate ingress/worker wait strategies.

  std::mutex error_mu;
  std::exception_ptr thread_error;
  auto recordError = [&](std::exception_ptr ep) {
    std::lock_guard<std::mutex> lock(error_mu);
    if (!thread_error) thread_error = ep;
  };

  const auto global_start = SteadyClock::now();
  const auto warmup_end = global_start + config.warmup;
  const auto measure_end = warmup_end + config.duration;
  state.measure_start_ns.store(
    std::chrono::duration_cast<std::chrono::nanoseconds>(warmup_end.time_since_epoch()).count(),
    std::memory_order_relaxed);

  std::atomic<bool> worker_stop{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(config.worker_threads));
  for (int worker_index = 0; worker_index < config.worker_threads; ++worker_index) {
    workers.emplace_back([&, worker_index] {
      std::uint64_t rng = 0x9e3779b97f4a7c15ULL ^ static_cast<std::uint64_t>(worker_index + 1);
      try {
        Event ev{};
        auto& metrics = worker_metrics[static_cast<std::size_t>(worker_index)];
        while (shared_queue->pop(ev, worker_stop)) {
          simulateWork(config.service_ns, config.jitter_ns, rng);
          if (isMeasuredEvent(ev, state)) {
            const auto now = SteadyClock::now();
            const auto latency_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(now - ev.created_at).count();
            state.completed.fetch_add(1, std::memory_order_relaxed);
            metrics.completed += 1;
            metrics.latencies_ns.push_back(static_cast<std::int64_t>(latency_ns));
          }
        }
      } catch (...) {
        recordError(std::current_exception());
        state.ingress_stop.store(true, std::memory_order_relaxed);
        shared_queue->close();
      }
    });
  }

  std::vector<std::thread> ingress_threads;
  ingress_threads.reserve(static_cast<std::size_t>(config.ingress_threads));
  const bool rate_limited = config.rate_per_ingress > 0.0;
  const auto interval = rate_limited
    ? std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(1.0 / config.rate_per_ingress))
    : SteadyClock::duration::zero();

  for (int ingress_index = 0; ingress_index < config.ingress_threads; ++ingress_index) {
    ingress_threads.emplace_back([&, ingress_index] {
      try {
        auto next_fire = SteadyClock::now();
        std::uint64_t seq = 0;
        while (!state.ingress_stop.load(std::memory_order_relaxed)) {
          const auto now = SteadyClock::now();
          if (now >= measure_end) break;

          if (rate_limited) {
            if (now < next_fire) {
              std::this_thread::sleep_until(next_fire);
              continue;
            }
            next_fire += interval;
          }

          Event ev{};
          ev.id = (static_cast<std::uint64_t>(ingress_index) << 48) | seq++;
          ev.route_key = ev.id;
          ev.target_worker = static_cast<std::size_t>(ev.route_key % static_cast<std::uint64_t>(config.worker_threads));
          ev.created_at = SteadyClock::now();

          const bool measured = isMeasuredTimestamp(ev.created_at, state);
          if (measured) {
            state.generated.fetch_add(1, std::memory_order_relaxed);
          }

          const bool ok = shared_queue->push(std::move(ev), config.policy, state.ingress_stop);
          if (ok) {
            if (measured) {
              state.accepted.fetch_add(1, std::memory_order_relaxed);
            }
          } else if (config.policy == BackpressurePolicy::Drop && measured) {
            state.dropped_ingress.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } catch (...) {
        recordError(std::current_exception());
        state.ingress_stop.store(true, std::memory_order_relaxed);
        shared_queue->close();
      }
    });
  }

  if (config.warmup.count() > 0) {
    std::this_thread::sleep_until(warmup_end);
  }
  std::this_thread::sleep_until(measure_end);

  state.ingress_stop.store(true, std::memory_order_relaxed);
  for (auto& thread : ingress_threads) thread.join();

  shared_queue->close();
  for (auto& thread : workers) thread.join();

  if (thread_error) std::rethrow_exception(thread_error);
  return finalizeResult(
    config,
    state,
    worker_metrics,
    snapshotQueueStats(shared_queue->stats()),
    QueueStatsSnapshot{});
}

PipelineResult runDispatchLocalSpscPipeline(const PipelineConfig& config) {
  PipelineState state;
  std::vector<WorkerMetrics> worker_metrics(static_cast<std::size_t>(config.worker_threads));
  auto dispatch_queue = makeQueue(
    config.dispatch_queue_kind, config.ingress_queue_capacity, QueueOptions{.wait_strategy = config.wait_strategy});

  std::vector<std::unique_ptr<IEventQueue>> worker_queues;
  worker_queues.reserve(static_cast<std::size_t>(config.worker_threads));
  for (int worker_index = 0; worker_index < config.worker_threads; ++worker_index) {
    (void)worker_index;
    worker_queues.push_back(makeQueue(QueueKind::SpscRing, config.worker_queue_capacity, QueueOptions{
                                                                                       .wait_strategy = config.wait_strategy,
                                                                                     }));
  }
  // dispatcher가 route 결정을 대신하는 local SPSC topology

  std::mutex error_mu;
  std::exception_ptr thread_error;
  auto recordError = [&](std::exception_ptr ep) {
    std::lock_guard<std::mutex> lock(error_mu);
    if (!thread_error) thread_error = ep;
  };

  const auto global_start = SteadyClock::now();
  const auto warmup_end = global_start + config.warmup;
  const auto measure_end = warmup_end + config.duration;
  state.measure_start_ns.store(
    std::chrono::duration_cast<std::chrono::nanoseconds>(warmup_end.time_since_epoch()).count(),
    std::memory_order_relaxed);

  std::atomic<bool> pop_stop{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(config.worker_threads));
  for (int worker_index = 0; worker_index < config.worker_threads; ++worker_index) {
    workers.emplace_back([&, worker_index] {
      std::uint64_t rng = 0x243f6a8885a308d3ULL ^ static_cast<std::uint64_t>(worker_index + 1);
      try {
        Event ev{};
        auto& metrics = worker_metrics[static_cast<std::size_t>(worker_index)];
        auto& queue = worker_queues[static_cast<std::size_t>(worker_index)];
        while (queue->pop(ev, pop_stop)) {
          simulateWork(config.service_ns, config.jitter_ns, rng);
          if (isMeasuredEvent(ev, state)) {
            const auto now = SteadyClock::now();
            const auto latency_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(now - ev.created_at).count();
            state.completed.fetch_add(1, std::memory_order_relaxed);
            metrics.completed += 1;
            metrics.latencies_ns.push_back(static_cast<std::int64_t>(latency_ns));
          }
        }
      } catch (...) {
        recordError(std::current_exception());
        state.ingress_stop.store(true, std::memory_order_relaxed);
        dispatch_queue->close();
        for (auto& queue : worker_queues) queue->close();
      }
    });
  }

  std::thread dispatcher([&] {
    try {
      Event ev{};
      while (dispatch_queue->pop(ev, pop_stop)) {
        auto target = ev.target_worker % static_cast<std::size_t>(config.worker_threads);
        auto& worker_queue = worker_queues[target];
        const bool measured = isMeasuredEvent(ev, state);
        const bool ok = worker_queue->push(std::move(ev), config.policy, pop_stop);
        if (!ok && config.policy == BackpressurePolicy::Drop && measured) {
          state.dropped_dispatch.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } catch (...) {
      recordError(std::current_exception());
      state.ingress_stop.store(true, std::memory_order_relaxed);
    }

    for (auto& queue : worker_queues) queue->close();
  });

  std::vector<std::thread> ingress_threads;
  ingress_threads.reserve(static_cast<std::size_t>(config.ingress_threads));
  const bool rate_limited = config.rate_per_ingress > 0.0;
  const auto interval = rate_limited
    ? std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(1.0 / config.rate_per_ingress))
    : SteadyClock::duration::zero();

  for (int ingress_index = 0; ingress_index < config.ingress_threads; ++ingress_index) {
    ingress_threads.emplace_back([&, ingress_index] {
      try {
        auto next_fire = SteadyClock::now();
        std::uint64_t seq = 0;
        while (!state.ingress_stop.load(std::memory_order_relaxed)) {
          const auto now = SteadyClock::now();
          if (now >= measure_end) break;

          if (rate_limited) {
            if (now < next_fire) {
              std::this_thread::sleep_until(next_fire);
              continue;
            }
            next_fire += interval;
          }

          Event ev{};
          ev.id = (static_cast<std::uint64_t>(ingress_index) << 48) | seq++;
          ev.route_key = ev.id;
          ev.target_worker = static_cast<std::size_t>(ev.route_key % static_cast<std::uint64_t>(config.worker_threads));
          ev.created_at = SteadyClock::now();

          const bool measured = isMeasuredTimestamp(ev.created_at, state);
          if (measured) {
            state.generated.fetch_add(1, std::memory_order_relaxed);
          }

          const bool ok = dispatch_queue->push(std::move(ev), config.policy, state.ingress_stop);
          if (ok) {
            if (measured) {
              state.accepted.fetch_add(1, std::memory_order_relaxed);
            }
          } else if (config.policy == BackpressurePolicy::Drop && measured) {
            state.dropped_ingress.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } catch (...) {
        recordError(std::current_exception());
        state.ingress_stop.store(true, std::memory_order_relaxed);
        dispatch_queue->close();
        for (auto& queue : worker_queues) queue->close();
      }
    });
  }

  if (config.warmup.count() > 0) {
    std::this_thread::sleep_until(warmup_end);
  }
  std::this_thread::sleep_until(measure_end);

  state.ingress_stop.store(true, std::memory_order_relaxed);
  for (auto& thread : ingress_threads) thread.join();

  dispatch_queue->close();
  dispatcher.join();
  for (auto& thread : workers) thread.join();

  if (thread_error) std::rethrow_exception(thread_error);
  QueueStatsSnapshot worker_queue_stats{};
  for (const auto& queue : worker_queues) {
    mergeQueueStats(worker_queue_stats, snapshotQueueStats(queue->stats()));
  }
  return finalizeResult(
    config,
    state,
    worker_metrics,
    snapshotQueueStats(dispatch_queue->stats()),
    worker_queue_stats);
}

PipelineResult runDirectLocalSpscPipeline(const PipelineConfig& config) {
  PipelineState state;
  std::vector<WorkerMetrics> worker_metrics(static_cast<std::size_t>(config.worker_threads));
  std::vector<std::unique_ptr<IEventQueue>> worker_queues;
  worker_queues.reserve(static_cast<std::size_t>(config.worker_threads));
  for (int worker_index = 0; worker_index < config.worker_threads; ++worker_index) {
    worker_queues.push_back(makeQueue(QueueKind::SpscRing, config.worker_queue_capacity, QueueOptions{
                                                                                       .wait_strategy = config.wait_strategy,
                                                                                     }));
  }

  std::mutex error_mu;
  std::exception_ptr thread_error;
  auto recordError = [&](std::exception_ptr ep) {
    std::lock_guard<std::mutex> lock(error_mu);
    if (!thread_error) thread_error = ep;
  };

  const auto global_start = SteadyClock::now();
  const auto warmup_end = global_start + config.warmup;
  const auto measure_end = warmup_end + config.duration;
  state.measure_start_ns.store(
    std::chrono::duration_cast<std::chrono::nanoseconds>(warmup_end.time_since_epoch()).count(),
    std::memory_order_relaxed);

  std::atomic<bool> pop_stop{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(config.worker_threads));
  for (int worker_index = 0; worker_index < config.worker_threads; ++worker_index) {
    workers.emplace_back([&, worker_index] {
      std::uint64_t rng = 0x517cc1b727220a95ULL ^ static_cast<std::uint64_t>(worker_index + 1);
      try {
        Event ev{};
        auto& metrics = worker_metrics[static_cast<std::size_t>(worker_index)];
        auto& queue = worker_queues[static_cast<std::size_t>(worker_index)];
        while (queue->pop(ev, pop_stop)) {
          simulateWork(config.service_ns, config.jitter_ns, rng);
          if (isMeasuredEvent(ev, state)) {
            const auto now = SteadyClock::now();
            const auto latency_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(now - ev.created_at).count();
            state.completed.fetch_add(1, std::memory_order_relaxed);
            metrics.completed += 1;
            metrics.latencies_ns.push_back(static_cast<std::int64_t>(latency_ns));
          }
        }
      } catch (...) {
        recordError(std::current_exception());
        state.ingress_stop.store(true, std::memory_order_relaxed);
        for (auto& queue : worker_queues) queue->close();
      }
    });
  }

  std::vector<std::thread> ingress_threads;
  ingress_threads.reserve(static_cast<std::size_t>(config.ingress_threads));
  const bool rate_limited = config.rate_per_ingress > 0.0;
  const auto interval = rate_limited
    ? std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(1.0 / config.rate_per_ingress))
    : SteadyClock::duration::zero();

  for (int ingress_index = 0; ingress_index < config.ingress_threads; ++ingress_index) {
    ingress_threads.emplace_back([&, ingress_index] {
      try {
        auto next_fire = SteadyClock::now();
        std::uint64_t seq = 0;
        // TODO(phase4): direct_local_spsc를 N:1 fan-in route matrix로 확장하면
        // ingress_threads <= worker_threads 제한 없이 direct route를 비교할 수 있다.
        const auto target_worker = static_cast<std::size_t>(ingress_index % config.worker_threads);
        auto& worker_queue = worker_queues[target_worker];
        while (!state.ingress_stop.load(std::memory_order_relaxed)) {
          const auto now = SteadyClock::now();
          if (now >= measure_end) break;

          if (rate_limited) {
            if (now < next_fire) {
              std::this_thread::sleep_until(next_fire);
              continue;
            }
            next_fire += interval;
          }

          Event ev{};
          ev.id = (static_cast<std::uint64_t>(ingress_index) << 48) | seq++;
          ev.route_key = ev.id;
          ev.target_worker = target_worker;
          ev.created_at = SteadyClock::now();

          const bool measured = isMeasuredTimestamp(ev.created_at, state);
          if (measured) {
            state.generated.fetch_add(1, std::memory_order_relaxed);
          }

          const bool ok = worker_queue->push(std::move(ev), config.policy, state.ingress_stop);
          if (ok) {
            if (measured) {
              state.accepted.fetch_add(1, std::memory_order_relaxed);
            }
          } else if (config.policy == BackpressurePolicy::Drop && measured) {
            state.dropped_ingress.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } catch (...) {
        recordError(std::current_exception());
        state.ingress_stop.store(true, std::memory_order_relaxed);
        for (auto& queue : worker_queues) queue->close();
      }
    });
  }

  if (config.warmup.count() > 0) {
    std::this_thread::sleep_until(warmup_end);
  }
  std::this_thread::sleep_until(measure_end);

  state.ingress_stop.store(true, std::memory_order_relaxed);
  for (auto& thread : ingress_threads) thread.join();

  for (auto& queue : worker_queues) queue->close();
  for (auto& thread : workers) thread.join();

  if (thread_error) std::rethrow_exception(thread_error);
  QueueStatsSnapshot worker_queue_stats{};
  for (const auto& queue : worker_queues) {
    mergeQueueStats(worker_queue_stats, snapshotQueueStats(queue->stats()));
  }
  return finalizeResult(
    config,
    state,
    worker_metrics,
    QueueStatsSnapshot{},
    worker_queue_stats);
}

} // namespace

PipelineTopology parsePipelineTopology(std::string_view name) {
  if (name == "shared" || name == "shared_queue" || name == "shared-queue") {
    return PipelineTopology::SharedQueue;
  }
  if (name == "dispatch_local_spsc" || name == "dispatch-local-spsc" || name == "dispatch_spsc" || name == "dispatch-spsc") {
    return PipelineTopology::DispatchLocalSpsc;
  }
  if (name == "direct_local_spsc" || name == "direct-local-spsc" || name == "direct_spsc" || name == "direct-spsc") {
    return PipelineTopology::DirectLocalSpsc;
  }
  throw std::runtime_error("topology는 현재 'shared', 'dispatch_local_spsc', 'direct_local_spsc'만 지원합니다.");
}

std::string_view pipelineTopologyName(PipelineTopology topology) noexcept {
  switch (topology) {
    case PipelineTopology::SharedQueue:
      return "shared_queue";
    case PipelineTopology::DispatchLocalSpsc:
      return "dispatch_local_spsc";
    case PipelineTopology::DirectLocalSpsc:
      return "direct_local_spsc";
  }
  return "unknown";
}

PipelineResult runPipelineScenario(const PipelineConfig& config) {
  validatePipelineConfig(config);

  switch (config.topology) {
    case PipelineTopology::SharedQueue:
      return runSharedQueuePipeline(config);
    case PipelineTopology::DispatchLocalSpsc:
      return runDispatchLocalSpscPipeline(config);
    case PipelineTopology::DirectLocalSpsc:
      return runDirectLocalSpscPipeline(config);
  }
  throw std::runtime_error("알 수 없는 pipeline topology입니다.");
}
