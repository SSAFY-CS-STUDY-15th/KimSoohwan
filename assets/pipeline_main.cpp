#include "pipeline.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void printUsage(const char* argv0) {
  std::cout
    << "Usage: " << argv0 << " [options]\n"
    << "\n"
    << "Options:\n"
    << "  --topology <shared|dispatch_local_spsc|direct_local_spsc>\n"
    << "  --shared-queue <global_lock|split_lock|mpmc_ring>\n"
    << "  --dispatch-queue <global_lock|split_lock|mpmc_ring>\n"
    << "  --ingress <n>               ingress thread 개수 (기본 4)\n"
    << "  --workers <n>               worker thread 개수 (기본 4)\n"
    << "  --duration-ms <ms>          측정 구간 길이 (기본 2000)\n"
    << "  --warmup-ms <ms>            워밍업 길이 (기본 0)\n"
    << "  --capacity <n>              shared/dispatch queue 용량 (기본 1024)\n"
    << "  --worker-capacity <n>       worker-local SPSC 용량 (기본 256)\n"
    << "  --rate <eps>                ingress당 초당 이벤트 수 (0이면 최대 속도)\n"
    << "  --policy <block|drop>\n"
    << "  --wait-strategy <default|cv|spin_yield>\n"
    << "  --service-ns <n>            worker 처리 시간 (기본 0)\n"
    << "  --jitter-ns <n>             worker 처리 지터 상한 (기본 0)\n"
    << "  --help\n";
}

bool isFlag(const std::string& s, const char* flag) {
  return s == flag;
}

std::string requireValue(int& i, int argc, char** argv, const char* flag) {
  if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " 값이 필요합니다.");
  ++i;
  return argv[i];
}

PipelineConfig parseArgs(int argc, char** argv) {
  PipelineConfig config{};

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (isFlag(arg, "--help") || isFlag(arg, "-h")) {
      printUsage(argv[0]);
      std::exit(0);
    }

    if (isFlag(arg, "--topology")) {
      config.topology = parsePipelineTopology(requireValue(i, argc, argv, "--topology"));
      continue;
    }
    if (isFlag(arg, "--shared-queue")) {
      config.shared_queue_kind = parseQueueKind(requireValue(i, argc, argv, "--shared-queue"));
      continue;
    }
    if (isFlag(arg, "--dispatch-queue")) {
      config.dispatch_queue_kind = parseQueueKind(requireValue(i, argc, argv, "--dispatch-queue"));
      continue;
    }
    if (isFlag(arg, "--ingress")) {
      config.ingress_threads = std::stoi(requireValue(i, argc, argv, "--ingress"));
      continue;
    }
    if (isFlag(arg, "--workers")) {
      config.worker_threads = std::stoi(requireValue(i, argc, argv, "--workers"));
      continue;
    }
    if (isFlag(arg, "--duration-ms")) {
      config.duration = std::chrono::milliseconds(std::stoi(requireValue(i, argc, argv, "--duration-ms")));
      continue;
    }
    if (isFlag(arg, "--warmup-ms")) {
      config.warmup = std::chrono::milliseconds(std::stoi(requireValue(i, argc, argv, "--warmup-ms")));
      continue;
    }
    if (isFlag(arg, "--capacity")) {
      config.ingress_queue_capacity = static_cast<std::size_t>(std::stoull(requireValue(i, argc, argv, "--capacity")));
      continue;
    }
    if (isFlag(arg, "--worker-capacity")) {
      config.worker_queue_capacity =
        static_cast<std::size_t>(std::stoull(requireValue(i, argc, argv, "--worker-capacity")));
      continue;
    }
    if (isFlag(arg, "--rate")) {
      config.rate_per_ingress = std::stod(requireValue(i, argc, argv, "--rate"));
      continue;
    }
    if (isFlag(arg, "--policy")) {
      const auto value = requireValue(i, argc, argv, "--policy");
      if (value == "block") config.policy = BackpressurePolicy::Block;
      else if (value == "drop") config.policy = BackpressurePolicy::Drop;
      else throw std::runtime_error("--policy는 block 또는 drop 이어야 합니다.");
      continue;
    }
    if (isFlag(arg, "--wait-strategy")) {
      config.wait_strategy = parseWaitStrategy(requireValue(i, argc, argv, "--wait-strategy"));
      continue;
    }
    if (isFlag(arg, "--service-ns")) {
      config.service_ns = std::stoull(requireValue(i, argc, argv, "--service-ns"));
      continue;
    }
    if (isFlag(arg, "--jitter-ns")) {
      config.jitter_ns = std::stoull(requireValue(i, argc, argv, "--jitter-ns"));
      continue;
    }

    throw std::runtime_error("알 수 없는 인자: " + arg);
  }

  return config;
}

void printResult(const PipelineConfig& config, const PipelineResult& result) {
  const auto policy_str = (config.policy == BackpressurePolicy::Block) ? "block" : "drop";
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "topology=" << pipelineTopologyName(config.topology)
            << " shared_queue=" << queueKindName(config.shared_queue_kind)
            << " dispatch_queue=" << queueKindName(config.dispatch_queue_kind)
            << " ingress_threads=" << config.ingress_threads
            << " worker_threads=" << config.worker_threads
            << " duration_ms=" << config.duration.count()
            << " warmup_ms=" << config.warmup.count()
            << " ingress_queue_capacity=" << config.ingress_queue_capacity
            << " worker_queue_capacity=" << config.worker_queue_capacity
            << " rate_per_ingress=" << config.rate_per_ingress
            << " policy=" << policy_str
            << " wait_strategy=" << waitStrategyName(config.wait_strategy)
            << " service_ns=" << config.service_ns
            << " jitter_ns=" << config.jitter_ns
            << "\n";

  std::cout << "tps_completed=" << result.tps_completed
            << " avg_us=" << result.avg_latency_us
            << " p95_us=" << result.p95_latency_us
            << " p99_us=" << result.p99_latency_us
            << " worker_completed_min=" << result.worker_completed_min
            << " worker_completed_max=" << result.worker_completed_max
            << " worker_completed_avg=" << result.worker_completed_avg
            << " worker_completed_stddev=" << result.worker_completed_stddev
            << " worker_imbalance_pct=" << result.worker_imbalance_pct
            << " generated=" << result.generated
            << " accepted=" << result.accepted
            << " completed=" << result.completed
            << " dropped=" << result.dropped
            << " dropped_ingress=" << result.dropped_ingress
            << " dropped_dispatch=" << result.dropped_dispatch
            << " ingress_push_attempts=" << result.ingress_queue_stats.push_attempts
            << " ingress_push_success=" << result.ingress_queue_stats.push_success
            << " ingress_push_fail=" << result.ingress_queue_stats.push_fail
            << " ingress_pop_success=" << result.ingress_queue_stats.pop_success
            << " ingress_spin_count=" << result.ingress_queue_stats.spin_count
            << " ingress_yield_count=" << result.ingress_queue_stats.yield_count
            << " ingress_park_count=" << result.ingress_queue_stats.park_count
            << " ingress_full_hits=" << result.ingress_queue_stats.full_hits
            << " ingress_empty_hits=" << result.ingress_queue_stats.empty_hits
            << " ingress_max_observed_depth=" << result.ingress_queue_stats.max_observed_depth
            << " worker_push_attempts=" << result.worker_queue_stats.push_attempts
            << " worker_push_success=" << result.worker_queue_stats.push_success
            << " worker_push_fail=" << result.worker_queue_stats.push_fail
            << " worker_pop_success=" << result.worker_queue_stats.pop_success
            << " worker_spin_count=" << result.worker_queue_stats.spin_count
            << " worker_yield_count=" << result.worker_queue_stats.yield_count
            << " worker_park_count=" << result.worker_queue_stats.park_count
            << " worker_full_hits=" << result.worker_queue_stats.full_hits
            << " worker_empty_hits=" << result.worker_queue_stats.empty_hits
            << " worker_max_observed_depth=" << result.worker_queue_stats.max_observed_depth
            << " samples=" << result.samples
            << "\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto config = parseArgs(argc, argv);
    const auto result = runPipelineScenario(config);
    printResult(config, result);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    printUsage(argv[0]);
    return 1;
  }
}
