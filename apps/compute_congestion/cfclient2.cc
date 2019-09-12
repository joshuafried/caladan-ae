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

barrier_t barrier;

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;

// <- ARGUMENTS FOR EXPERIMENT ->
// the number of worker threads to spawn.
int threads;
double offered_load;
// the remote UDP address of the server.
netaddr raddr;
// the mean service time in us.
double st;
// number of iterations required for 1us on target server
constexpr uint64_t kIterationsPerUS = 88;
// Number of seconds to warmup at rate 0
constexpr uint64_t kWarmupUpSeconds = 5;

constexpr uint64_t kExperimentDuration = 5000000;
constexpr uint64_t kSLOUS = 50000;

constexpr uint64_t MAX_USERID = 610;

static std::vector<std::pair<double, uint64_t>> rates;

static float ntohf(float value) {
  union v {
    float f;
    unsigned int i;
  };

  union v val;
  unsigned int temp;

  val.f = value;
  temp = ntoh32(val.i);

  return *(float*)&temp;
}

constexpr uint64_t kTBMaxToken = 32; // reqs
constexpr uint64_t kTBMinTimeToSleep = 1; // us
constexpr uint64_t kTBMaxTimeToSleep = 1000; // us
class TokenBucket {
public:
  TokenBucket(uint64_t rate)
    : refresh_interval_(1000000000 / rate), token_(0),
      clock_(steady_clock::now()) { }

  void Update() {
    barrier();
    auto now = steady_clock::now();
    barrier();

    uint64_t elapsed_time_ns = duration_cast<nanoseconds>(now - clock_).count();

    if (elapsed_time_ns >= refresh_interval_) {
      int new_token = elapsed_time_ns / refresh_interval_;
      token_ += new_token;
      token_ = std::min<uint64_t>(token_, kTBMaxToken);
      clock_ += nanoseconds(new_token * refresh_interval_);
    }
  }

  bool RetrieveToken() {
    if (token_ > 0) {
      token_--;
      return true;
    }
    return false;
  }

  void SetRate(uint64_t rate) {
    refresh_interval_ = 1000000000 / rate;
  }

  uint64_t GetRefreshInterval() {
    return refresh_interval_;
  }

  // return request rate (req / s)
  uint64_t GetRate() {
    return (1000000000 / refresh_interval_);
  }

  void SleepUntilNextToken() {
    barrier();
    auto now = steady_clock::now();
    barrier();

    uint64_t elapsed_time_ns = duration_cast<nanoseconds>(now - clock_).count();

    if (elapsed_time_ns < refresh_interval_) {
      uint64_t time_to_sleep_us = (refresh_interval_ - elapsed_time_ns) / 1000 + 1;
      time_to_sleep_us = std::min<uint64_t>(time_to_sleep_us, kTBMaxTimeToSleep);
      rt::Sleep(time_to_sleep_us);
    } 
  }

  void EmptyToken() {
    barrier();
    auto now = steady_clock::now();
    barrier();
    token_ = 0;
    clock_ = now;
  }

private:
  // token refill time (ns/req)
  uint64_t refresh_interval_;
  // the number of remaining token (reqs)
  uint64_t token_; 
  // internal timer
  time_point<steady_clock> clock_;
};

constexpr uint64_t kNetbenchPort = 8001;
struct payload {
  uint64_t user_id;
  uint64_t movie_id;
  uint64_t request_index;
  uint64_t queueing_delay;
  uint64_t processing_time;
  float rating;
};

// The maximum lateness to tolerate before dropping egress samples.
constexpr uint64_t kMaxCatchUpUS = 5;

struct work_unit {
  double start_us, duration_us, latency_us;
  uint64_t user_id;
  uint64_t movie_id;
  uint64_t timing;
  float rating;
};

template <class Arrival, class Service>
std::vector<work_unit> GenerateWork(Arrival a, Service s, double cur_us,
                                    double last_us) {
  std::vector<work_unit> w;

  while (cur_us < last_us) {
    cur_us += a();
    w.emplace_back(work_unit{cur_us, 0, 0,
        (rand() % 10 + 1), (rand() % 10 + 1), 0});
  }
  return w;
}

std::vector<work_unit> ClientWorker(
    rt::TcpConn *c, rt::WaitGroup *starter,
    std::function<std::vector<work_unit>()> wf,
    int worker_id,
    std::vector<std::pair<uint64_t, uint64_t>> &queues,
    std::vector<std::pair<uint64_t, double>> &cwnds) {
  constexpr int kBatchSize = 32;
  std::vector<work_unit> w(wf());
  std::vector<time_point<steady_clock>> timings;
  timings.reserve(w.size());

  // window-based CC

  time_point<steady_clock> expstart;

  // Start the receiver thread.
  auto th = rt::Thread([&] {
    payload rp;

    while (true) {
      ssize_t ret = c->ReadFull(&rp, sizeof(rp));
      if (ret != static_cast<ssize_t>(sizeof(rp))) {
        if (ret == 0 || ret < 0) break;
        panic("read failed, ret = %ld", ret);
      }

      barrier();
      auto ts = steady_clock::now();
      barrier();
      uint64_t idx = ntoh64(rp.request_index);

      // execution time
      w[idx].duration_us = duration_cast<sec>(ts - timings[idx]).count();
      // execution time + client queueing delay
      w[idx].latency_us = duration_cast<sec>(ts - expstart).count() - w[idx].start_us;
      w[idx].rating = ntohf(rp.rating);
    }
  });

  // Synchronized start of load generation.
  starter->Done();
  starter->Wait();

  barrier();
  expstart = steady_clock::now();
  barrier();

  payload p[kBatchSize];
  int j = 0;
  auto wsize = w.size();

  for (unsigned int i = 0; i < wsize; ++i) {
    barrier();
    auto now = steady_clock::now();
    barrier();

    if (duration_cast<sec>(now - expstart).count() > kExperimentDuration) break;

    if (duration_cast<sec>(now - expstart).count() < w[i].start_us) {
      ssize_t ret = c->WriteFull(p, sizeof(payload) * j);
      if (ret != static_cast<ssize_t>(sizeof(payload) * j))
        panic("write failed, ret = %ld", ret);
      j = 0;
      now = steady_clock::now();
      rt::Sleep(w[i].start_us - duration_cast<sec>(now - expstart).count());
    }

    if (j > 0) {
      ssize_t ret = c->WriteFull(p, sizeof(payload) * j);
      if (ret != static_cast<ssize_t>(sizeof(payload) * j))
        panic("write failed, ret = %ld", ret);
      j = 0;
    }

    barrier();
    timings[i] = steady_clock::now();
    barrier();

    w[i].timing = duration_cast<sec>(timings[i] - expstart).count();

    // Enqueue a network request.
    p[j].user_id = hton64(w[i].user_id);
    p[j].movie_id = hton64(w[i].movie_id);
    p[j].request_index = hton64(i);
    j++;

    if (j >= kBatchSize || i >= wsize - 1) {
      ssize_t ret = c->WriteFull(p, sizeof(payload) * j);
      if (ret != static_cast<ssize_t>(sizeof(payload) * j))
        panic("write failed, ret = %ld", ret);
      j = 0;
    }
  }

  c->Shutdown(SHUT_RDWR);
  th.Join();

  return w;
}

std::vector<work_unit> RunExperiment(
    int threads, double *cpu_usage,
    std::function<std::vector<work_unit>()> wf) {
  // Create one TCP connection per thread.
  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  for (int i = 0; i < threads; ++i) {
    std::unique_ptr<rt::TcpConn> outc(rt::TcpConn::Dial({0, 0}, raddr));
    if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
    conns.emplace_back(std::move(outc));
  }

  // Launch a worker thread for each connection.
  rt::WaitGroup starter(threads + 1);
  std::vector<rt::Thread> th;
  std::unique_ptr<std::vector<work_unit>> samples[threads];
  std::vector<std::pair<uint64_t, uint64_t>> queues;
  std::vector<std::pair<uint64_t, double>> cwnds;
  for (int i = 0; i < threads; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      auto v = ClientWorker(conns[i].get(), &starter, wf, i, queues, cwnds);
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
//  uptime u1 = ReadUptime();

  // Wait for the workers to finish.
  for (auto &t : th) t.Join();

  // |--- end experiment duration timing ---|
  barrier();
  auto finish = steady_clock::now();
  barrier();
//  uptime u2 = ReadUptime();

  // Close the connections.
  for (auto &c : conns) c->Abort();

  // Aggregate all the samples together.
  std::vector<work_unit> w;
  for (int i = 0; i < threads; ++i) {
    auto &v = *samples[i];
    w.insert(w.end(), v.begin(), v.end());
  }
/*
// Per-flow group throughput
  std::vector<work_unit> vs[threads];
  for (int i=0; i<threads; ++i) {
    auto &v = *samples[i];
    vs[i].insert(vs[i].end(), v.begin(), v.end());
  }


  for (int i=0; i < threads; ++i) {
    vs[i].erase(std::remove_if(vs[i].begin(), vs[i].end(),
                              [](const work_unit &s) { return s.timing == 0; }),
                vs[i].end());
    std::sort(vs[i].begin(), vs[i].end(),
              [](const work_unit &s1, work_unit &s2) { return s1.timing < s2.timing; });
  }

  int num_data_point = 500;
  int granularity = 10;
  double throughput[num_data_point][2];

  for (int i=0; i<num_data_point; ++i) {
    for(int j=0; j<2; ++j) {
      throughput[i][j] = 0.0;
    }
  }

  for (int i=0; i < threads; ++i) {
    int num_req_out = 0;
    uint64_t next_target = granularity;
    int idx = 0;
    for (const work_unit &u : vs[i]) {
      if (u.timing <= next_target) {
        num_req_out++;
      } else {
        throughput[idx][i%2] += double(num_req_out)/double(granularity);
        do {
          idx++;
          next_target = (idx+1)*granularity;
        } while (u.timing + granularity > next_target);
        num_req_out = 1;
      }
      if (idx >= num_data_point) break;
    }
  }

  for (int i = 0; i < num_data_point; ++i) {
    std::cout << (i+1)*granularity/1000.0;
    for (int j=0; j<2; ++j) {
      std::cout << "," << throughput[i][j];
    }
    std::cout << std::endl;
  }
*/
/*
// Print aggregated throughput
  w.erase(std::remove_if(w.begin(), w.end(),
                         [](const work_unit &s) { return s.timing == 0; }),
          w.end());

  std::sort(w.begin(), w.end(),
            [](const work_unit &s1, work_unit &s2) { return s1.timing < s2.timing; });

  int num_req_out = 0;
  int granularity = 10;
  uint64_t next_target = granularity;
  std::ofstream agt_out;
  agt_out.open("agt.out");

  for (const work_unit &u : w) {
    if (u.timing <= next_target) {
      num_req_out++;
    } else {
      agt_out << next_target/1000.0 << "," << num_req_out/double(granularity) << std::endl;
      next_target += granularity;
      while (u.timing > next_target) {
        agt_out << next_target/1000.0 << ",0" << std::endl;
        next_target += granularity;
      }
      num_req_out = 1;
    }
    if (next_target > 10000) break;
  }
  agt_out.close();
*/
/*
  // Print queue info for flow 0
  std::ofstream q_out;
  q_out.open("q.out");
  for (auto &q : queues) {
    if (q.first > 10000) break;
    q_out << q.first / 1000.0 << "," << q.second << std::endl;
  }
  q_out.close();
*/
/*
  // Print cwnd info for flow 0
  std::ofstream cwnd_out;
  cwnd_out.open("cwnd.out");
  for (auto &c : cwnds) {
    if (c.first > 10000) break;
    cwnd_out << c.first / 1000.0 << "," << c.second << std::endl;
  }
  cwnd_out.close();
*/
//  uint64_t idle = u2.idle - u1.idle;
//  uint64_t busy = u2.busy - u1.busy;
//  if (cpu_usage != nullptr)
//    *cpu_usage = static_cast<double>(busy) / static_cast<double>(idle + busy);
  return w;
}

void PrintStatResultsDuration(std::vector<work_unit> w, double offered_rps, double rps,
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
  double p9999 = w[count * 0.9999].duration_us;
  double min = w[0].duration_us;
  double max = w[w.size() - 1].duration_us;
  std::cout  //<<
             //"#threads,offered_rps,rps,cpu_usage,samples,min,mean,p90,p99,p999,p9999,max"
             //<< std::endl
      << std::setprecision(4) << std::fixed << threads << "," << offered_rps
      << "," << rps << "," << cpu_usage << "," << w.size() << "," << min << ","
      << mean << "," << p90 << "," << p99 << "," << p999 << "," << p9999 << ","
      << max << std::endl;
}

void PrintStatResultsLatency(std::vector<work_unit> w, double offered_rps,
                             double cpu_usage) {
  double total = static_cast<double>(w.size());

  // Remove requests that did not complete.
  w.erase(std::remove_if(w.begin(), w.end(),
                         [](const work_unit &s) { return s.duration_us == 0; }),
          w.end());

  double rps = static_cast<double>(w.size()) / (double)kExperimentDuration * 1000000;

  std::sort(w.begin(), w.end(), [](const work_unit &s1, work_unit &s2) {
    return s1.latency_us < s2.latency_us;
  });
  double sum = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.latency_us; });
  double mean = sum / w.size();
  double count = static_cast<double>(w.size());
  double p90 = w[count * 0.9].latency_us;
  double p99 = w[count * 0.99].latency_us;
  double p999 = w[count * 0.999].latency_us;
  double p9999 = w[count * 0.9999].latency_us;
  double min = w[0].latency_us;
  double max = w[w.size() - 1].latency_us;

  double slo_success = static_cast<double>(std::count_if(w.begin(), w.end(), 
                           [](const work_unit &s) { return s.latency_us < kSLOUS; }));
  double gps = slo_success / (double)kExperimentDuration * 1000000;
  double slo_rate = slo_success / total;

  double sum_goodput = std::accumulate(
      w.begin(), w.end(), 0.0,
      [] (double s, const work_unit &c) { if (c.latency_us < kSLOUS) return s + c.rating; else return s; });

  double pgps = sum_goodput / (kExperimentDuration / 1000000.0);

  std::cout  //<<
             //"#threads,offered_rps,rps,cpu_usage,samples,min,mean,p90,p99,p999,p9999,max"
             //<< std::endl
      << std::setprecision(4) << std::fixed << threads << "," << offered_rps
      << "," << rps << "," << gps << "," << pgps << "," << cpu_usage << "," << slo_rate
      << "," << w.size() << "," << min << "," << mean << "," << p90
      << "," << p99 << "," << p999 << "," << p9999 << "," << max << std::endl;
}

double GetBimodalRandom(std::mt19937 &rgen) {
  if (rgen() > (unsigned int)0xe6666665) {
    return 1000.0;
  } else {
    return 10.0;
  }
}

void SteadyStateExperiment(int threads, double offered_rps,
                           double service_time) {
  double cpu_usage;
  std::vector<work_unit> w = RunExperiment(threads, &cpu_usage, [=] {
    std::mt19937 rg(rand());
    std::mt19937 dg(rand());
    std::exponential_distribution<double> rd(
        1.0 / (1000000.0 / (offered_rps / static_cast<double>(threads))));
    std::exponential_distribution<double> wd(1.0 / service_time);
    return GenerateWork(std::bind(rd, rg), std::bind(wd, dg), 0, kExperimentDuration);
  });

  // Print the results.
  PrintStatResultsLatency(w, offered_rps, cpu_usage);
}

void ClientHandler(void *arg) {
    SteadyStateExperiment(threads, offered_load, st);
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
  if (cmd.compare("client") != 0) {
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
  offered_load = std::stod(argv[6], nullptr);
/*
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
*/

  ret = runtime_init(argv[1], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
