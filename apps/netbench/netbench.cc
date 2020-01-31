extern "C" {
#include <base/log.h>
#include <net/ip.h>
#include <unistd.h>
}

#include "net.h"
#include "runtime.h"
#include "sync.h"
#include "synthetic_worker.h"
#include "thread.h"
#include "timer.h"
#include "rpc.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

barrier_t barrier;

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;

// <- ARGUMENTS FOR EXPERIMENT ->
// the number of worker threads to spawn.
int threads;
// the remote UDP address of the server.
netaddr raddr;
// the mean service time in us.
double st;
// number of iterations required for 1us on target server
constexpr uint64_t kIterationsPerUS = 69;  // 83
// Number of seconds to warmup at rate 0
constexpr uint64_t kWarmupUpSeconds = 5;

static std::vector<std::pair<double, uint64_t>> rates;

std::vector<double> offered_loads;

SyntheticWorker **workers;

constexpr uint64_t kUptimePort = 8002;
constexpr uint64_t kUptimeMagic = 0xDEADBEEF;
struct uptime {
  uint64_t idle;
  uint64_t busy;
};

void UptimeWorker(std::unique_ptr<rt::TcpConn> c) {
  while (true) {
    // Receive an uptime request.
    uint64_t magic;
    ssize_t ret = c->ReadFull(&magic, sizeof(magic));
    if (ret != static_cast<ssize_t>(sizeof(magic))) {
      if (ret == 0 || ret == -ECONNRESET) break;
      log_err("read failed, ret = %ld", ret);
      break;
    }

    // Check for the right magic value.
    if (ntoh64(magic) != kUptimeMagic) break;

    // Calculate the current uptime.
    std::ifstream file("/proc/stat");
    std::string line;
    std::getline(file, line);
    std::istringstream ss(line);
    std::string tmp;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest,
        guest_nice;
    ss >> tmp >> user >> nice >> system >> idle >> iowait >> irq >> softirq >>
        steal >> guest >> guest_nice;
    uptime u = {hton64(idle + iowait),
                hton64(user + nice + system + irq + softirq + steal)};

    // Send an uptime response.
    ssize_t sret = c->WriteFull(&u, sizeof(u));
    if (sret != sizeof(u)) {
      if (sret == -EPIPE || sret == -ECONNRESET) break;
      log_err("write failed, ret = %ld", sret);
      break;
    }
  }
}

void UptimeServer() {
  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen({0, kUptimePort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { UptimeWorker(std::unique_ptr<rt::TcpConn>(c)); }).Detach();
  }
}

uptime ReadUptime() {
  std::unique_ptr<rt::TcpConn> c(
      rt::TcpConn::Dial({0, 0}, {raddr.ip, kUptimePort}));
  uint64_t magic = hton64(kUptimeMagic);
  ssize_t ret = c->WriteFull(&magic, sizeof(magic));
  if (ret != static_cast<ssize_t>(sizeof(magic)))
    panic("uptime request failed, ret = %ld", ret);
  uptime u;
  ret = c->ReadFull(&u, sizeof(u));
  if (ret != static_cast<ssize_t>(sizeof(u)))
    panic("uptime response failed, ret = %ld", ret);
  return uptime{ntoh64(u.idle), ntoh64(u.busy)};
}

constexpr uint64_t kNetbenchPort = 8001;
struct payload {
  uint64_t work_iterations;
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t queueing;
};

// The maximum lateness to tolerate before dropping egress samples.
constexpr uint64_t kMaxCatchUpUS = 5;

void RpcServer(struct srpc_ctx *ctx) {
  // Validate and parse the request.
  if (unlikely(ctx->req_len != sizeof(payload))) {
    log_err("got invalid RPC len %ld", ctx->req_len);
    return;
  }
  const payload *in = reinterpret_cast<const payload *>(ctx->req_buf);

  // Perform the synthetic work.
  uint64_t workn = ntoh64(in->work_iterations);
  int core_id = get_current_affinity();
  SyntheticWorker* w = workers[core_id];

  if (workn != 0) w->Work(workn);

  // Craft a response.
  ctx->resp_len = sizeof(payload);
  payload *out = reinterpret_cast<payload *>(ctx->resp_buf);
  memcpy(out, in, sizeof(*out));
  out->tsc_end = hton64(rdtscp(&out->cpu));
  out->cpu = hton32(out->cpu);
  out->queueing = hton64(rt::RuntimeStandingQueueUS());
}

void ServerHandler(void *arg) {
  rt::Thread([] { UptimeServer(); }).Detach();
  int num_cores = rt::RuntimeMaxCores();

  workers = (SyntheticWorker **)malloc(sizeof(SyntheticWorker*) * num_cores);
  for (int i = 0; i < num_cores; ++i) {
    SyntheticWorker* w = SyntheticWorkerFactory("stridedmem:3200:64");
    if (w == nullptr) printf("cannot create worker\n");
    workers[i] = w;
  }

  int ret = rt::RpcServerEnable(RpcServer);
  if (ret) panic("couldn't enable RPC server");

  // waits forever.
  rt::WaitGroup(1).Wait();
}

struct work_unit {
  double start_us, work_us, duration_us;
  uint64_t window;
  uint64_t tsc;
  uint32_t cpu;
  uint64_t queueing;
};

template <class Arrival, class Service>
std::vector<work_unit> GenerateWork(Arrival a, Service s, double cur_us,
                                    double last_us) {
  std::vector<work_unit> w;
  while (cur_us < last_us) {
    cur_us += a();
    w.emplace_back(work_unit{cur_us, s(), 0});
  }
  return w;
}

std::vector<work_unit> ClientWorker(
    rt::RpcClient *c, rt::WaitGroup *starter,
    std::function<std::vector<work_unit>()> wf) {
  std::vector<work_unit> w(wf());
  std::vector<time_point<steady_clock>> timings;
  timings.reserve(w.size());

  // Start the receiver thread.
  auto th = rt::Thread([&] {
    payload rp;

    while (true) {
      ssize_t ret = c->Recv(&rp, sizeof(rp));
      if (ret != static_cast<ssize_t>(sizeof(rp))) {
        if (ret == 0 || ret < 0) break;
        panic("read failed, ret = %ld", ret);
      }

      barrier();
      auto ts = steady_clock::now();
      barrier();
      uint64_t idx = ntoh64(rp.index);
      w[idx].duration_us = duration_cast<sec>(ts - timings[idx]).count();
      w[idx].window = c->WinAvail();
      w[idx].tsc = ntoh64(rp.tsc_end);
      w[idx].cpu = ntoh32(rp.cpu);
      w[idx].queueing = ntoh64(rp.queueing);
    }
  });

  // Synchronized start of load generation.
  starter->Done();
  starter->Wait();

  barrier();
  auto expstart = steady_clock::now();
  barrier();

  payload p;
  auto wsize = w.size();

  for (unsigned int i = 0; i < wsize; ++i) {
    barrier();
    auto now = steady_clock::now();
    barrier();
    if (duration_cast<sec>(now - expstart).count() < w[i].start_us) {
      rt::Sleep(w[i].start_us - duration_cast<sec>(now - expstart).count());
    }
    if (duration_cast<sec>(now - expstart).count() - w[i].start_us >
        kMaxCatchUpUS)
      continue;

    barrier();
    timings[i] = steady_clock::now();
    barrier();

    // Send an RPC request.
    p.work_iterations = hton64(w[i].work_us * kIterationsPerUS);
    p.index = hton64(i);
    ssize_t ret = c->Send(&p, sizeof(p));
    if (ret == -ENOBUFS)
      continue;
    if (ret != static_cast<ssize_t>(sizeof(p)))
      panic("write failed, ret = %ld", ret);
  }

  // rt::Sleep(1 * rt::kSeconds);
  c->Shutdown(SHUT_RDWR);
  th.Join();

  return w;
}

std::vector<work_unit> RunExperiment(
    int threads, double *reqs_per_sec, double *cpu_usage,
    std::function<std::vector<work_unit>()> wf) {
  // Create one TCP connection per thread.
  std::vector<std::unique_ptr<rt::RpcClient>> conns;
  for (int i = 0; i < threads; ++i) {
    std::unique_ptr<rt::RpcClient> outc(rt::RpcClient::Dial(raddr));
    if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
    conns.emplace_back(std::move(outc));
  }

  // Launch a worker thread for each connection.
  rt::WaitGroup starter(threads + 1);
  std::vector<rt::Thread> th;
  std::unique_ptr<std::vector<work_unit>> samples[threads];
  for (int i = 0; i < threads; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      auto v = ClientWorker(conns[i].get(), &starter, wf);
      samples[i].reset(new std::vector<work_unit>(std::move(v)));
    }));
  }

  // Give the workers time to initialize, then start recording.
  starter.Done();
  starter.Wait();

  // |--- start experiment duration timing ---|
  barrier();
  auto start = steady_clock::now();
  barrier();
  uptime u1 = ReadUptime();

  // Wait for the workers to finish.
  for (auto &t : th) t.Join();

  // |--- end experiment duration timing ---|
  barrier();
  auto finish = steady_clock::now();
  barrier();
  uptime u2 = ReadUptime();

  // Force the connections to close.
  for (auto &c : conns) c->Abort();

  // Aggregate all the samples together.
  std::vector<work_unit> w;
  for (int i = 0; i < threads; ++i) {
    auto &v = *samples[i];
    w.insert(w.end(), v.begin(), v.end());
  }

  // Remove requests that did not complete.
  w.erase(std::remove_if(w.begin(), w.end(),
                         [](const work_unit &s) { return s.duration_us == 0; }),
          w.end());

  // Report results.
  double elapsed = duration_cast<sec>(finish - start).count();
  if (reqs_per_sec != nullptr)
    *reqs_per_sec = static_cast<double>(w.size()) / elapsed * 1000000;
  uint64_t idle = u2.idle - u1.idle;
  uint64_t busy = u2.busy - u1.busy;
  if (cpu_usage != nullptr)
    *cpu_usage = static_cast<double>(busy) / static_cast<double>(idle + busy);
  return w;
}

void PrintRawResults(std::vector<work_unit> w) {
  std::sort(w.begin(), w.end(),
            [](const work_unit &s1, work_unit &s2) { return s1.tsc < s2.tsc; });
  for (const work_unit &u : w) {
    std::cout << std::setprecision(2) << std::fixed << u.start_us << ","
              << u.duration_us << "," << u.work_us << "," << u.tsc << ","
              << u.cpu << std::endl;
  }
}

void PrintStatResults(std::vector<work_unit> w, double offered_rps, double rps,
                      double cpu_usage) {
  std::sort(w.begin(), w.end(), [](const work_unit &s1, work_unit &s2) {
    return s1.duration_us < s2.duration_us;
  });
  double sum = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.duration_us; });
  double mean = sum / w.size();
  double count = static_cast<double>(w.size());
  double p50 = w[count * 0.5].duration_us;
  double p90 = w[count * 0.9].duration_us;
  double p99 = w[count * 0.99].duration_us;
  double p999 = w[count * 0.999].duration_us;
  double p9999 = w[count * 0.9999].duration_us;
  double min = w[0].duration_us;
  double max = w[w.size() - 1].duration_us;

  double sum_win = std::accumulate(
	w.begin(), w.end(), 0.0,
	[](double s, const work_unit &c) {return s + c.window; });
  double mean_win = sum_win / w.size();

  double sum_que = std::accumulate(
	w.begin(), w.end(), 0.0,
	[](double s, const work_unit &c) {return s + c.queueing; });
  double mean_que = sum_que / w.size();

  std::cout  //<<
             //"#threads,offered_rps,rps,cpu_usage,samples,min,mean,p90,p99,p999,p9999,max"
             //<< std::endl
      << std::setprecision(4) << std::fixed << threads << "," << offered_rps
      << "," << rps << "," << cpu_usage << "," << w.size() << "," << min << ","
      << mean << "," << p50 << "," <<  p90 << "," << p99 << "," << p999 << ","
      << p9999 << "," << max << "," << mean_win << "," << mean_que << std::endl;
}

void SteadyStateExperiment(int threads, double offered_rps,
                           double service_time) {
  double rps, cpu_usage;
  std::vector<work_unit> w = RunExperiment(threads, &rps, &cpu_usage, [=] {
    std::mt19937 rg(rand());
    std::mt19937 dg(rand());
    std::exponential_distribution<double> rd(
        1.0 / (1000000.0 / (offered_rps / static_cast<double>(threads))));
    std::exponential_distribution<double> wd(1.0 / service_time);
    return GenerateWork(std::bind(rd, rg), std::bind(wd, dg), 0, 2000000);
  });

  // Print the results.
  PrintStatResults(w, offered_rps, rps, cpu_usage);
}

void LoadShiftExperiment(int threads,
                         const std::vector<std::pair<double, uint64_t>> &rates,
                         double service_time) {
  auto w = RunExperiment(threads, nullptr, nullptr, [=] {
    std::mt19937 rg(rand());
    std::mt19937 wg(rand());
    std::exponential_distribution<double> wd(1.0 / service_time);
    std::vector<work_unit> w1;
    uint64_t last_us = 0;
    for (auto &r : rates) {
      std::exponential_distribution<double> rd(
          1.0 / (1000000.0 / (r.first / static_cast<double>(threads))));
      auto work = GenerateWork(std::bind(rd, rg), std::bind(wd, wg), last_us,
                               last_us + r.second);
      last_us = work.back().start_us;
      w1.insert(w1.end(), work.begin(), work.end());
    }
    return w1;
  });
  PrintRawResults(w);
}

void ClientHandler(void *arg) {
  // LoadShiftExperiment(threads, rates, st);
#if 1
  for (double i : offered_loads) {
    SteadyStateExperiment(threads, i, st);
  }
#endif
}

int StringToAddr(const char *str, uint32_t *addr) {
  uint8_t a, b, c, d;

  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;

  *addr = MAKE_IP_ADDR(a, b, c, d);
  return 0;
}

std::vector<std::string> split(const std::string &text, char sep) {
  std::vector<std::string> tokens;
  std::string::size_type start = 0, end = 0;
  while ((end = text.find(sep, start)) != std::string::npos) {
    tokens.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  tokens.push_back(text.substr(start));
  return tokens;
}

}  // anonymous namespace

int main(int argc, char *argv[]) {
  int i, ret;

  if (argc < 3) {
    std::cerr << "usage: [cfg_file] [cmd] ..." << std::endl;
    return -EINVAL;
  }



  std::string cmd = argv[2];
  if (cmd.compare("server") == 0) {
    ret = runtime_init(argv[1], ServerHandler, NULL);
    if (ret) {
      printf("failed to start runtime\n");
      return ret;
    }
  } else if (cmd.compare("client") != 0) {
    std::cerr << "invalid command: " << cmd << std::endl;
    return -EINVAL;
  }

  if (argc < 7) {
    std::cerr << "usage: [cfg_file] client [#threads] [remote_ip] [service_us] "
                 "[<request_rate>:<us_duration>]..."
              << std::endl;
    return -EINVAL;
  }

  threads = std::stoi(argv[3], nullptr, 0);

  ret = StringToAddr(argv[4], &raddr.ip);
  if (ret) return -EINVAL;
  raddr.port = kNetbenchPort;

  st = std::stod(argv[5], nullptr);

  for (i = 6; i < argc; i++) {
    std::vector<std::string> tokens = split(argv[i], ':');
    if (tokens.size() != 2) return -EINVAL;
    double rate = std::stod(tokens[0], nullptr);
    uint64_t duration = std::stoll(tokens[1], nullptr, 0);
#if 0
    if (i == 6) {
      rates.emplace_back(rate, kWarmupUpSeconds * 1e6);
    }
#endif
    rates.emplace_back(rate, duration);
  }

  for(double l = 50000; l <= 2000000; l += 50000)
    offered_loads.push_back(l);

  for(double l = 2500000; l <= 8000000; l += 500000)
    offered_loads.push_back(l);

  ret = runtime_init(argv[1], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
