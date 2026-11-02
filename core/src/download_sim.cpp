#include "download_sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace panplay {
namespace {

constexpr double kStep = 0.01;
constexpr double kTick = 0.25;
constexpr double kMB = 1024.0 * 1024.0;

class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

 private:
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t state_;
};

struct SimLane {
  enum class State { idle, connecting, receiving };
  State state = State::idle;
  DownloadScheduler::Lease lease;
  double ready_at = 0;
  double factor = 1;
  std::int64_t got = 0;
  std::int64_t stall_after = -1;
  std::int64_t error_after = -1;
  double carry = 0;
};

std::string fixed(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", value);
  return text;
}

double total_at(const SimScenario& scenario, double t) {
  return (scenario.total_at ? scenario.total_at(t) : scenario.total_mbps) * kMB;
}

std::int64_t total_size(const SimScenario& scenario) {
  std::int64_t size = 0;
  for (const std::int64_t extent : scenario.extents) size += extent;
  return size;
}

double ideal_seconds(const SimScenario& scenario) {
  const std::int64_t size = total_size(scenario);
  const double lanes = scenario.options.adaptive ? 8 : std::clamp(scenario.options.max_lanes, 1, 16);
  double done = 0;
  double t = 0;
  while (done < static_cast<double>(size) && t < scenario.time_limit) {
    const double rate = std::min({scenario.consume_mbps * kMB, total_at(scenario, t), scenario.lane_mbps * kMB * lanes});
    done += rate * kStep;
    t += kStep;
  }
  return t;
}

}  // namespace

SimResult run_download_sim(const SimScenario& scenario) {
  SimResult result;
  Random random(scenario.seed);
  const std::int64_t size = total_size(scenario);
  DownloadScheduler scheduler(scenario.extents, scenario.options, scenario.memory, 0, 0.0);
  std::vector<SimLane> lanes(static_cast<std::size_t>(std::clamp(scenario.options.max_lanes, 1, 16)));
  const std::string fault = "模拟的连接出错";
  double t = 0;
  double next_tick = kTick;
  double consume_carry = 0;
  std::size_t seek_index = 0;
  int lanes_seen = scheduler.target_lanes();
  result.most_lanes = lanes_seen;
  std::string path = std::to_string(lanes_seen);

  const auto drain = [&] {
    for (const auto& event : scheduler.take_events()) {
      if (event.kind == DownloadScheduler::Event::Kind::abort) {
        if (event.lane >= 0 && event.lane < static_cast<int>(lanes.size())) {
          lanes[static_cast<std::size_t>(event.lane)].state = SimLane::State::idle;
        }
        continue;
      }
      result.log.push_back(std::string(event.kind == DownloadScheduler::Event::Kind::info ? "" : "  · ") + "[" + fixed(t) +
                           "s] " + event.text);
    }
    if (scheduler.target_lanes() != lanes_seen) {
      lanes_seen = scheduler.target_lanes();
      ++result.lane_changes;
      result.most_lanes = std::max(result.most_lanes, lanes_seen);
      path += "→" + std::to_string(lanes_seen);
    }
  };
  const auto release = [&](std::size_t k, const std::string* error) {
    scheduler.released(static_cast<int>(k), lanes[k].lease.id, error, t);
    lanes[k].state = SimLane::State::idle;
  };

  while (true) {
    if (scheduler.position() >= size) break;
    if (!scheduler.error().empty()) {
      result.error = "下载出错：" + scheduler.error();
      break;
    }
    if (t > scenario.time_limit) {
      result.error = "超时，读到 " + fixed(static_cast<double>(scheduler.position()) / kMB) + " MB";
      break;
    }
    for (std::size_t k = 0; k < lanes.size(); ++k) {
      SimLane& lane = lanes[k];
      if (lane.state != SimLane::State::idle) continue;
      DownloadScheduler::Lease lease;
      double wake_at = 0;
      if (scheduler.next(static_cast<int>(k), t, &lease, &wake_at) != DownloadScheduler::Step::lease) continue;
      lane = SimLane{};
      lane.state = SimLane::State::connecting;
      lane.lease = lease;
      lane.ready_at = t + scenario.first_byte + scenario.first_byte_jitter * random.uniform();
      const std::int64_t length = lease.end - lease.start;
      lane.factor = random.uniform() < scenario.straggler_chance ? scenario.straggler_factor : 1.0;
      if (random.uniform() < scenario.stall_chance) lane.stall_after = static_cast<std::int64_t>(random.uniform() * length);
      if (random.uniform() < scenario.error_chance) lane.error_after = static_cast<std::int64_t>(random.uniform() * length);
    }
    drain();
    for (std::size_t k = 0; k < lanes.size(); ++k) {
      SimLane& lane = lanes[k];
      if (lane.state == SimLane::State::connecting && t >= lane.ready_at) {
        scheduler.connected(static_cast<int>(k), lane.lease.id, t);
        lane.state = SimLane::State::receiving;
      }
    }
    // 共享带宽按水位线分：每条要的不超过平均份额就全给，剩下的再平分给要得多的。
    std::vector<std::pair<double, std::size_t>> demand;
    std::vector<std::int64_t> allowance(lanes.size(), 0);
    for (std::size_t k = 0; k < lanes.size(); ++k) {
      SimLane& lane = lanes[k];
      if (lane.state != SimLane::State::receiving) continue;
      allowance[k] = scheduler.allowance(static_cast<int>(k), lane.lease.id, t);
      if (allowance[k] < 0) {
        release(k, nullptr);
        continue;
      }
      double want = scenario.lane_mbps * kMB * lane.factor;
      if (lane.stall_after >= 0 && lane.got >= lane.stall_after) want = 0;
      if (allowance[k] > 0 && want > 0) demand.emplace_back(want, k);
    }
    std::sort(demand.begin(), demand.end());
    double remaining = total_at(scenario, t);
    for (std::size_t i = 0; i < demand.size(); ++i) {
      const auto [want, k] = demand[i];
      const double give = std::min(want, remaining / static_cast<double>(demand.size() - i));
      remaining -= give;
      SimLane& lane = lanes[k];
      const double bytes = give * kStep + lane.carry;
      auto n = static_cast<std::int64_t>(bytes);
      lane.carry = bytes - static_cast<double>(n);
      n = std::min(n, allowance[k]);
      if (lane.stall_after >= 0) n = std::min(n, std::max<std::int64_t>(0, lane.stall_after - lane.got));
      if (lane.error_after >= 0 && lane.got + n >= lane.error_after) {
        n = lane.error_after - lane.got;
        if (n > 0) scheduler.received(static_cast<int>(k), lane.lease.id, n, t);
        release(k, &fault);
        continue;
      }
      if (n <= 0) continue;
      lane.got += n;
      scheduler.received(static_cast<int>(k), lane.lease.id, n, t);
      if (scheduler.allowance(static_cast<int>(k), lane.lease.id, t) < 0) release(k, nullptr);
    }
    drain();

    const std::int64_t position = scheduler.position();
    if (seek_index < scenario.seeks.size() && position >= scenario.seeks[seek_index].first) {
      scheduler.seek(scenario.seeks[seek_index].second, t);
      ++seek_index;
      consume_carry = 0;
      drain();
    } else {
      const double budget = scenario.consume_mbps * kMB * kStep + consume_carry;
      const auto wanted = static_cast<std::int64_t>(budget);
      const std::int64_t got = std::min(wanted, scheduler.available() - position);
      if (got > 0) scheduler.consumed(position + got, t);
      consume_carry = got == wanted ? budget - static_cast<double>(wanted) : 0;
      scheduler.set_waiting(got < wanted && position + got < size, t);
    }
    if (t >= next_tick) {
      scheduler.tick(t);
      next_tick += kTick;
      drain();
    }
    if (const std::string problem = scheduler.check(); !problem.empty()) {
      result.error = "不变式不成立：" + problem;
      break;
    }
    t += kStep;
  }
  result.seconds = t;
  result.bound = ideal_seconds(scenario);
  result.totals = scheduler.totals();
  result.final_lanes = scheduler.target_lanes();
  result.memory = scheduler.memory();
  result.log.insert(result.log.begin(), "条数 " + path);
  return result;
}

std::string describe_sim(const SimScenario& scenario, const SimResult& result) {
  const auto& totals = result.totals;
  std::string text = scenario.name + "：" + (result.error.empty() ? "" : result.error + "，") + "用时 " + fixed(result.seconds) +
                     " 秒（理想 " + fixed(result.bound) + "，多 " +
                     std::to_string(static_cast<int>((result.seconds / std::max(result.bound, 0.01) - 1) * 100 + 0.5)) +
                     "%），等数据 " + std::to_string(static_cast<int>(totals.wait / std::max(totals.elapsed, 0.01) * 100 + 0.5)) +
                     "%，请求 " + std::to_string(totals.requests) + "，分走 " + std::to_string(totals.steals) + "，断开 " +
                     std::to_string(totals.aborts) + "，出错 " + std::to_string(totals.failures) + "，丢弃 " +
                     fixed(static_cast<double>(totals.discarded) / kMB) + " MB，最后 " + std::to_string(result.final_lanes) +
                     " 条（记下 " + std::to_string(result.memory.lanes) + " 条、每条 " + fixed(result.memory.lane_rate / kMB) +
                     " MB/s）\n";
  for (const std::string& line : result.log) {
    if (line.rfind("  · ", 0) != 0) text += "    " + line + "\n";
  }
  return text;
}

std::vector<SimScenario> download_sim_scenarios() {
  constexpr std::int64_t MB = 1ll << 20;
  std::vector<SimScenario> out;
  SimScenario base;

  SimScenario single = base;
  single.name = "单条够快（每条 40，总 40，解压 30）";
  single.extents = {200 * MB, 200 * MB, 200 * MB};
  single.lane_mbps = 40;
  single.total_mbps = 40;
  single.consume_mbps = 30;
  out.push_back(single);

  SimScenario capped = base;
  capped.name = "每条限速（每条 10，总 60）";
  capped.extents = {3000 * MB};
  capped.lane_mbps = 10;
  capped.total_mbps = 60;
  out.push_back(capped);

  SimScenario shared = base;
  shared.name = "共享瓶颈（每条 50，总 12）";
  shared.extents = {1500 * MB};
  shared.lane_mbps = 50;
  shared.total_mbps = 12;
  out.push_back(shared);

  SimScenario consumer = base;
  consumer.name = "解压慢（每条 10，总 100，解压 15，记下 4 条）";
  consumer.extents = {1500 * MB};
  consumer.lane_mbps = 10;
  consumer.consume_mbps = 15;
  consumer.memory.lanes = 4;
  consumer.memory.lane_rate = 10 * MB;
  out.push_back(consumer);

  SimScenario fixed4 = base;
  fixed4.name = "固定 4 条（每条 10，没有故障）";
  fixed4.extents = {2000 * MB};
  fixed4.options.adaptive = false;
  fixed4.options.max_lanes = 4;
  out.push_back(fixed4);

  SimScenario stragglers = fixed4;
  stragglers.name = "慢连接（固定 4 条，每条 10，5% 的请求只有一成速度）";
  stragglers.straggler_chance = 0.05;
  out.push_back(stragglers);

  SimScenario stalls = stragglers;
  stalls.name = "卡住（固定 4 条，每条 10，3% 的请求中途卡死）";
  stalls.straggler_chance = 0;
  stalls.stall_chance = 0.03;
  out.push_back(stalls);

  SimScenario volumes = base;
  volumes.name = "分卷（10 卷 × 64 MB，每条 40，总 40）";
  volumes.extents.assign(10, 64 * MB);
  volumes.lane_mbps = 40;
  volumes.total_mbps = 40;
  out.push_back(volumes);

  SimScenario remembered = base;
  remembered.name = "记下 6 条但这次共享瓶颈（每条 50，总 12）";
  remembered.extents = {1500 * MB};
  remembered.lane_mbps = 50;
  remembered.total_mbps = 12;
  remembered.memory.lanes = 6;
  remembered.memory.lane_rate = 10 * MB;
  out.push_back(remembered);

  SimScenario wave = base;
  wave.name = "总带宽在 20 和 58 之间来回（每条 10）";
  wave.extents = {3000 * MB};
  wave.lane_mbps = 10;
  wave.total_at = [](double t) { return std::fmod(t, 30.0) < 15 ? 58.0 : 20.0; };
  out.push_back(wave);

  SimScenario seeks = base;
  seeks.name = "跳转（三卷，每条 10，总 40）";
  seeks.extents = {300 * MB, 400 * MB, 300 * MB};
  seeks.lane_mbps = 10;
  seeks.total_mbps = 40;
  seeks.seeks = {{100 * MB, 300 * MB}, {400 * MB, 380 * MB}, {420 * MB, 423 * MB}, {500 * MB, 900 * MB}, {950 * MB, 10 * MB}};
  out.push_back(seeks);

  SimScenario errors = base;
  errors.name = "出错重试（每条 10，总 40，5% 的请求中途出错）";
  errors.extents = {1000 * MB};
  errors.lane_mbps = 10;
  errors.total_mbps = 40;
  errors.error_chance = 0.05;
  out.push_back(errors);
  return out;
}

}  // namespace panplay
