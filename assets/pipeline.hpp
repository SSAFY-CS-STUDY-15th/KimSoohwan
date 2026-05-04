#pragma once

#include "queues.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

enum class PipelineTopology {
  SharedQueue,
  DispatchLocalSpsc,
  DirectLocalSpsc,
};

struct PipelineConfig {
  int ingress_threads = 4;
  int worker_threads = 4;
  std::chrono::milliseconds duration{2000};
  std::chrono::milliseconds warmup{0};
  std::size_t ingress_queue_capacity = 1024;
  std::size_t worker_queue_capacity = 256;
  double rate_per_ingress = 0.0;
  BackpressurePolicy policy = BackpressurePolicy::Block;
  WaitStrategy wait_strategy = WaitStrategy::QueueDefault;
  PipelineTopology topology = PipelineTopology::SharedQueue;
  QueueKind shared_queue_kind = QueueKind::SplitLockLinked;
  QueueKind dispatch_queue_kind = QueueKind::SplitLockLinked;
  std::uint64_t service_ns = 0;
  std::uint64_t jitter_ns = 0;
};

struct PipelineResult {
  double tps_completed = 0.0;
  double avg_latency_us = 0.0;
  double p95_latency_us = 0.0;
  double p99_latency_us = 0.0;
  double worker_completed_avg = 0.0;
  double worker_completed_stddev = 0.0;
  double worker_imbalance_pct = 0.0;

  std::uint64_t generated = 0;
  std::uint64_t accepted = 0;
  std::uint64_t completed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t dropped_ingress = 0;
  std::uint64_t dropped_dispatch = 0;
  std::uint64_t worker_completed_min = 0;
  std::uint64_t worker_completed_max = 0;
  QueueStatsSnapshot ingress_queue_stats{};
  QueueStatsSnapshot worker_queue_stats{};

  std::chrono::nanoseconds elapsed{0};
  std::size_t samples = 0;
};

PipelineTopology parsePipelineTopology(std::string_view name);
std::string_view pipelineTopologyName(PipelineTopology topology) noexcept;
PipelineResult runPipelineScenario(const PipelineConfig& config);
