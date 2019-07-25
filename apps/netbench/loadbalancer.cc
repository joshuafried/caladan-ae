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

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;

// <- ARGUMENTS FOR EXPERIMENT ->
// the number of worker threads to spawn
int num_threads;
// the number of remote server
int num_servers;
// the remote address of the server
std::vector<netaddr> raddrs;
// the mean service time in us.
double st;
// the number of iterations required for 1us on target server
constexpr uint64_t kIterationsPerUS = 88;

constexpr uint64_t kLoadBalancerPort = 8001;
struct payload {
  uint64_t work_iterations;
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t standing_queue_us;
};

constexpr uint64_t kMaxCatchUpUS = 5;

void ServerWorker(std::unique_ptr<rt::TcpConn> c) {
  payload p;
  std::unique_ptr<SyntheticWorker> w(
      SyntheticWorkerFactory("stridedmem:3200:64"));
  if (w == nullptr) panic("couldn't create worker");

  while (true) {
    // receive a work request
    ssize_t ret = c->ReadFull(&p, sizeof(p));
    if (ret != static_cast<ssize_t>(sizeof(p))) {
      if (ret == 0 || ret == -ECONNRESET) break;
      log_err("read failed, ret = %ld", ret);
      break;
    }

    // Perform fake work if requested.
    uint64_t workn = ntoh64(p.work_iterations);
    if (workn != 0) w->Work(workn);
    p.tsc_end = hton64(rdtscp(&p.cpu));
    p.cpu = hton32(p.cpu);
    p.standing_queue_us = hton64(rt::RuntimeStandingQueueUS());

    // Send a work response
    ssize_t sret = c->WriteFull(&p, ret);
    if (sret != ret) {
      if (sret == -EPIPE || sret == -ECONNRESET) break;
      log_err("write failed, ret = %ld", sret);
      break;
    }
  }
}

void ServerHandler(void *arg) {
  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kLoadBalancerPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ServerWorker(std::unique_ptr<rt::TcpConn>(c)); }).Detach();
  }
}

struct work_unit {
  double start_us, work_us, duration_us;
  uint64_t timing;
  uint64_t tsc;
  uint32_t cpu;
};

template <class Arrival, class Service>
std::vector<work_unit> GenerateWork(Arrival a, Service s, double cur_us,
                                    double last_us) {
  std::vector<work_unit> w;
  while (cur_us < last_us) {
    cur_us += a();
    w.emplace_back(work_unit{cur_us, s(), 0, 0});
  }
  return w;
}

std::vector<work_unit> ClientWorker(
    std::vector<rt::TcpConn*> cs,
    int num_servers, rt::WaitGroup *starter,
    std::function<std::vector<work_unit>()> wf,
    int worker_id) {
  std::vector<work_unit> w(wf());
  std::vector<time_point<steady_clock>> timings;
  timings.reserve(w.size());

  std::vector<uint64_t> metrics;
  metrics.reserve(num_servers);
  for(int i = 0; i < num_servers; ++i)
    metrics[i] = 0;

  std::vector<rt::Thread> th;
  for (int i = 0; i < num_servers; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      payload rp;

      while (true) {
        ssize_t ret = cs[i]->ReadFull(&rp, sizeof(rp));
        if (ret != static_cast<ssize_t>(sizeof(rp))) {
          if (ret == 0 || ret < 0) break;
          panic("read failed, ret = %ld", ret);
        }

        barrier();
        auto ts = steady_clock::now();
        barrier();
        uint64_t idx = ntoh64(rp.index);
        w[idx].duration_us = duration_cast<sec>(ts - timings[idx]).count();
        w[idx].tsc = ntoh64(rp.tsc_end);
        w[idx].cpu = ntoh32(rp.cpu);

        uint64_t standing_queue_us = ntoh64(rp.standing_queue_us);
      }
    }));
  }

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

    w[i].timing = duration_cast<sec>(timings[i] - expstart).count();

    p.work_iterations = hton64(w[i].work_us * kIterationsPerUS);
    p.index = hton64(i);

    rt::TcpConn* c= cs[0];

    ssize_t ret = c->WriteFull(&p, sizeof(payload));
    if (ret != static_cast<ssize_t>(sizeof(payload)))
      panic("write failed, ret = %ld", ret);
  }

  for (auto &c : cs) c->Shutdown(SHUT_RDWR);
  for (auto &t : th) t.Join();

  return w;
}

std::vector<work_unit> RunExperiment(
    int num_threads, int num_servers, double *reqs_per_sec, double *cpu_usage,
    std::function<std::vector<work_unit>()> wf) {
  // Create TCP connections
  //
  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  for (int i = 0; i < num_threads; ++i) {
    for (int j = 0; j < num_servers; ++j) {
      std::unique_ptr<rt::TcpConn> outc(rt::TcpConn::Dial({0, 0}, raddrs[j]));
      if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
      conns.emplace_back(std::move(outc));
    }
  }

  // Launch a worker thread for each connection
  rt::WaitGroup starter(num_threads + 1);
  std::vector<rt::Thread> th;
  std::unique_ptr<std::vector<work_unit>> samples[num_threads];
  for (int i = 0; i < num_threads; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      std::vector<rt::TcpConn*> cs_;
      for(int j = 0 ; j < num_servers ; ++j) {
        cs_.push_back(conns[num_servers*i+j].get());
      }      
      auto v = ClientWorker(cs_, num_servers, &starter, wf, i);
      samples[i].reset(new std::vector<work_unit>(std::move(v)));
    }));
  }

  starter.Done();
  starter.Wait();

  barrier();
  auto start = steady_clock::now();
  barrier();

  for (auto &t : th) t.Join();

  barrier();
  auto finish = steady_clock::now();
  barrier();

  for (auto &c : conns) c->Abort();

  std::vector<work_unit> w;
  for (int i = 0; i < num_threads; ++i) {
    auto &v = *samples[i];
    w.insert(w.end(), v.begin(), v.end());
  }

  w.erase(std::remove_if(w.begin(), w.end(),
                         [](const work_unit &s) { return s.duration_us == 0; }),
          w.end());

  double elapsed = duration_cast<sec>(finish - start).count();
  if (reqs_per_sec != nullptr)
    *reqs_per_sec = static_cast<double>(w.size()) / elapsed * 1000000;

  return w;
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
  double p90 = w[count * 0.9].duration_us;
  double p99 = w[count * 0.99].duration_us;
  double p999 = w[count * 0.999].duration_us;
  double min = w[0].duration_us;
  double max = w[w.size() - 1].duration_us;
  std::cout
    << std::setprecision(4) << std::fixed << num_threads << "," << offered_rps
    << "," << rps << "," << cpu_usage << "," << w.size() << "," << min << ","
    << mean << "," << p90 << "," << p99 << "," << p999 << "," << max << std::endl;
}

void SteadyStateExperiment(int num_threads, int num_servers, double offered_rps,
                           double service_time) {
  double rps, cpu_usage;
  std::vector<work_unit> w = RunExperiment(num_threads, num_servers, &rps, &cpu_usage, [=] {
      std::mt19937 rg(rand());
      std::mt19937 dg(rand());
      std::exponential_distribution<double> rd(
          1.0 / (1000000.0 / (offered_rps / static_cast<double>(num_threads))));
      std::exponential_distribution<double> wd(1.0 / service_time);
      return GenerateWork(std::bind(rd, rg), std::bind(wd, dg), 0, 5000000);
  });

  PrintStatResults(w, offered_rps, rps, cpu_usage);
}

void ClientHandler(void *arg) {
  for (double i = 100000; i <= 100000; i += 100000) {
    SteadyStateExperiment(num_threads, num_servers, i, st);
  }
}

int StringToAddr(const char *str, uint32_t *addr) {
  uint8_t a, b, c, d;

  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;

  *addr = MAKE_IP_ADDR(a, b, c, d);
  return 0;
}

} // anonymous namespace


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
    std::cerr << "usage: [cfg_file] client [#threads] [service_us] "
                 "[num_server] [server_ip#1] ..."
              << std::endl;
    return -EINVAL;
  }

  num_threads = std::stoi(argv[3], nullptr, 0);
  st = std::stod(argv[4], nullptr);
  num_servers = std::stoi(argv[5], nullptr, 0);

  for (i = 0; i < num_servers; ++i) {
    netaddr raddr;
    ret = StringToAddr(argv[6+i], &raddr.ip);
    if (ret) return -EINVAL;
    raddr.port = kLoadBalancerPort;
    raddrs.push_back(raddr);
  }

  ret = runtime_init(argv[1], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
