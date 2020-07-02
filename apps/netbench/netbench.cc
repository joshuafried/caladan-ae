extern "C" {
#include <base/time.h>
#include <base/log.h>
#include <net/ip.h>
#include <unistd.h>
}

#include "net.h"
#include "rpc.h"
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
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define REPORT_TIME_SERIES 0
#define TIME_SERIES_GRAN 10000

#include <ctime>
std::time_t timex;

barrier_t barrier;

constexpr uint16_t kBarrierPort = 41;

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;

// <- ARGUMENTS FOR EXPERIMENT ->
// the number of worker threads to spawn.
int threads;
// the remote UDP address of the server.
netaddr raddr, master;
// the mean service time in us.
double st;

std::ofstream json_out;
std::ofstream csv_out;
#if REPORT_TIME_SERIES
std::ofstream tseries_out;
#endif

int total_agents = 1;
// number of iterations required for 1us on target server
constexpr uint64_t kIterationsPerUS = 69;  // 83
// Total duration of the experiment in us
constexpr uint64_t kWarmUpTime = 2000000;
constexpr uint64_t kExperimentTime = 4000000;
// RTT
constexpr uint64_t kRTT = 10;
constexpr int STRICT_SLO = 300;

std::vector<double> offered_loads;
double offered_load;

static SyntheticWorker *workers[NCPU];

/* server-side stat */
constexpr uint64_t kRPCSStatPort = 8002;
constexpr uint64_t kRPCSStatMagic = 0xDEADBEEF;
struct sstat_raw {
  uint64_t idle;
  uint64_t busy;
  unsigned int num_cores;
  unsigned int max_cores;
  uint64_t winu_rx;
  uint64_t winu_tx;
  uint64_t win_tx;
  uint64_t req_rx;
  uint64_t req_dropped;
  uint64_t resp_tx;
};

constexpr uint64_t kShenangoStatPort = 40;
constexpr uint64_t kShenangoStatMagic = 0xDEADBEEF;
struct shstat_raw {
  uint64_t rx_pkts;
  uint64_t tx_pkts;
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint64_t drops;
  uint64_t rx_tcp_ooo;
};

struct sstat {
  double cpu_usage;
  double rx_pps;
  double tx_pps;
  double rx_bps;
  double tx_bps;
  double rx_drops_pps;
  double rx_ooo_pps;
  double winu_rx_pps;
  double winu_tx_pps;
  double win_tx_wps;
  double req_rx_pps;
  double req_drop_rate;
  double resp_tx_pps;
};

/* client-side stat */
struct cstat_raw {
  double offered_rps;
  double rps;
  double goodput;
  double min_percli_tput;
  double max_percli_tput;
  uint64_t winu_rx;
  uint64_t winu_tx;
  uint64_t resp_rx;
  uint64_t req_tx;
  uint64_t win_expired;
  uint64_t req_dropped;
};

struct cstat {
  double offered_rps;
  double rps;
  double goodput;
  double min_percli_tput;
  double max_percli_tput;
  double winu_rx_pps;
  double winu_tx_pps;
  double resp_rx_pps;
  double req_tx_pps;
  double win_expired_wps;
  double req_dropped_rps;
};

struct work_unit {
  double start_us, work_us, duration_us, latency_us;
  uint64_t window;
  uint64_t tsc;
  uint32_t cpu;
  uint64_t server_queue;
  uint64_t server_time;
  uint64_t client_queue;
};

class NetBarrier {
 public:
  static constexpr uint64_t npara = 10;
  NetBarrier(int npeers) {
    threads /= total_agents;

    is_leader_ = true;
    std::unique_ptr<rt::TcpQueue> q(
        rt::TcpQueue::Listen({0, kBarrierPort}, 4096));
    aggregator_ = std::move(std::unique_ptr<rt::TcpQueue>(
        rt::TcpQueue::Listen({0, kBarrierPort + 1}, 4096)));
    for (int i = 0; i < npeers; i++) {
      rt::TcpConn *c = q->Accept();
      if (c == nullptr) panic("couldn't accept a connection");
      conns.emplace_back(c);
      BUG_ON(c->WriteFull(&threads, sizeof(threads)) <= 0);
      BUG_ON(c->WriteFull(&st, sizeof(st)) <= 0);
      BUG_ON(c->WriteFull(&raddr, sizeof(raddr)) <= 0);
      BUG_ON(c->WriteFull(&total_agents, sizeof(total_agents)) <= 0);
      for (size_t j = 0; j < npara; j++) {
        rt::TcpConn *c = aggregator_->Accept();
        if (c == nullptr) panic("couldn't accept a connection");
        agg_conns_.emplace_back(c);
      }
    }
  }

  NetBarrier(netaddr leader) {
    auto c = rt::TcpConn::Dial({0, 0}, {leader.ip, kBarrierPort});
    if (c == nullptr) panic("barrier");
    conns.emplace_back(c);
    is_leader_ = false;
    BUG_ON(c->ReadFull(&threads, sizeof(threads)) <= 0);
    BUG_ON(c->ReadFull(&st, sizeof(st)) <= 0);
    BUG_ON(c->ReadFull(&raddr, sizeof(raddr)) <= 0);
    BUG_ON(c->ReadFull(&total_agents, sizeof(total_agents)) <= 0);
    for (size_t i = 0; i < npara; i++) {
      auto c = rt::TcpConn::Dial({0, 0}, {master.ip, kBarrierPort + 1});
      BUG_ON(c == nullptr);
      agg_conns_.emplace_back(c);
    }
  }

  bool Barrier() {
    char buf[1];
    if (is_leader_) {
      for (auto &c : conns) {
        if (c->ReadFull(buf, 1) != 1) return false;
      }
      for (auto &c : conns) {
        if (c->WriteFull(buf, 1) != 1) return false;
      }
    } else {
      if (conns[0]->WriteFull(buf, 1) != 1) return false;
      if (conns[0]->ReadFull(buf, 1) != 1) return false;
    }
    return true;
  }

  bool StartExperiment() { return Barrier(); }

  bool EndExperiment(std::vector<work_unit> &w, struct cstat_raw *csr) {
    if (is_leader_) {
      for (auto &c : conns) {
	struct cstat_raw rem_csr;
	BUG_ON(c->ReadFull(&rem_csr, sizeof(rem_csr)) <= 0);
	csr->offered_rps += rem_csr.offered_rps;
	csr->rps += rem_csr.rps;
	csr->goodput += rem_csr.goodput;
	csr->min_percli_tput = MIN(rem_csr.min_percli_tput,
				   csr->min_percli_tput);
	csr->max_percli_tput = MAX(rem_csr.max_percli_tput,
				   csr->max_percli_tput);
	csr->winu_rx += rem_csr.winu_rx;
	csr->winu_tx += rem_csr.winu_tx;
	csr->resp_rx += rem_csr.resp_rx;
	csr->req_tx += rem_csr.req_tx;
	csr->win_expired += rem_csr.win_expired;
	csr->req_dropped += rem_csr.req_dropped;
      }
    } else {
      BUG_ON(conns[0]->WriteFull(csr, sizeof(*csr)) <= 0);
    }
    GatherSamples(w);
    BUG_ON(!Barrier());
    return is_leader_;
  }

  bool IsLeader() {
    return is_leader_;
  }

 private:
  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  std::unique_ptr<rt::TcpQueue> aggregator_;
  std::vector<std::unique_ptr<rt::TcpConn>> agg_conns_;
  bool is_leader_;

  void GatherSamples(std::vector<work_unit> &w) {
    std::vector<rt::Thread> th;
    if (is_leader_) {
      std::unique_ptr<std::vector<work_unit>> samples[agg_conns_.size()];
      for (size_t i = 0; i < agg_conns_.size(); ++i) {
        th.emplace_back(rt::Thread([&, i] {
          size_t nelem;
          BUG_ON(agg_conns_[i]->ReadFull(&nelem, sizeof(nelem)) <= 0);

	  if (likely(nelem > 0)) {
            work_unit *wunits = new work_unit[nelem];
            BUG_ON(agg_conns_[i]->ReadFull(wunits, sizeof(work_unit) * nelem) <=
                   0);
            std::vector<work_unit> v(wunits, wunits + nelem);
            delete[] wunits;

            samples[i].reset(new std::vector<work_unit>(std::move(v)));
	  } else {
	    samples[i].reset(new std::vector<work_unit>());
	  }
        }));
      }

      for (auto &t : th) t.Join();
      for (size_t i = 0; i < agg_conns_.size(); ++i) {
        auto &v = *samples[i];
        w.insert(w.end(), v.begin(), v.end());
      }
    } else {
      for (size_t i = 0; i < agg_conns_.size(); ++i) {
        th.emplace_back(rt::Thread([&, i] {
          size_t elems = w.size() / npara;
          work_unit *start = w.data() + elems * i;
          if (i == npara - 1) elems += w.size() % npara;
          BUG_ON(agg_conns_[i]->WriteFull(&elems, sizeof(elems)) <= 0);
	  if (likely(elems > 0))
	    BUG_ON(agg_conns_[i]->WriteFull(start, sizeof(work_unit) * elems)
	           <= 0);
        }));
      }
      for (auto &t : th) t.Join();
    }
  }
};

static NetBarrier *b;

void RPCSStatWorker(std::unique_ptr<rt::TcpConn> c) {
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
    if (ntoh64(magic) != kRPCSStatMagic) break;

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
    sstat_raw u = {idle + iowait,
                   user + nice + system + irq + softirq + steal,
	           rt::RuntimeMaxCores(),
	           static_cast<unsigned int>(sysconf(_SC_NPROCESSORS_ONLN)),
	           rt::RpcServerStatWinuRx(),
	           rt::RpcServerStatWinuTx(),
		   rt::RpcServerStatWinTx(),
	           rt::RpcServerStatReqRx(),
		   rt::RpcServerStatReqDropped(),
	           rt::RpcServerStatRespTx()};

    // Send an uptime response.
    ssize_t sret = c->WriteFull(&u, sizeof(u));
    if (sret != sizeof(u)) {
      if (sret == -EPIPE || sret == -ECONNRESET) break;
      log_err("write failed, ret = %ld", sret);
      break;
    }
  }
}

void RPCSStatServer() {
  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen({0, kRPCSStatPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { RPCSStatWorker(std::unique_ptr<rt::TcpConn>(c)); }).Detach();
  }
}

sstat_raw ReadRPCSStat() {
  std::unique_ptr<rt::TcpConn> c(
      rt::TcpConn::Dial({0, 0}, {raddr.ip, kRPCSStatPort}));
  uint64_t magic = hton64(kRPCSStatMagic);
  ssize_t ret = c->WriteFull(&magic, sizeof(magic));
  if (ret != static_cast<ssize_t>(sizeof(magic)))
    panic("sstat request failed, ret = %ld", ret);
  sstat_raw u;
  ret = c->ReadFull(&u, sizeof(u));
  if (ret != static_cast<ssize_t>(sizeof(u)))
    panic("sstat response failed, ret = %ld", ret);
  return sstat_raw{u.idle, u.busy, u.num_cores, u.max_cores, u.winu_rx,
                   u.winu_tx, u.win_tx, u.req_rx, u.req_dropped, u.resp_tx};
}

shstat_raw ReadShenangoStat() {
  char *buf_;
  std::string buf;
  std::map<std::string, uint64_t> smap;
  std::unique_ptr<rt::TcpConn> c(
      rt::TcpConn::Dial({0,0}, {raddr.ip, kShenangoStatPort}));
  uint64_t magic = hton64(kShenangoStatMagic);
  ssize_t ret = c->WriteFull(&magic, sizeof(magic));
  if (ret != static_cast<ssize_t>(sizeof(magic)))
    panic("Shenango stat request failed, ret = %ld", ret);

  size_t resp_len;
  ret = c->ReadFull(&resp_len, sizeof(resp_len));
  if (ret != static_cast<ssize_t>(sizeof(resp_len)))
    panic("Shenango stat response failed, ret = %ld", ret);

  buf_ = (char *)malloc(resp_len);

  ret = c->ReadFull(buf_, resp_len);
  if (ret != static_cast<ssize_t>(resp_len))
    panic("Shenango stat response failed, ret = %ld", ret);

  buf = std::string(buf_);

  size_t pos_com = 0;
  size_t pos_col = 0;
  std::string token;
  std::string key;
  uint64_t value;

  while ((pos_com = buf.find(",")) != std::string::npos) {
    token = buf.substr(0, pos_com);
    pos_col = token.find(":");
    if (pos_col == std::string::npos)
      continue;

    key = token.substr(0, pos_col);
    value = std::stoull(token.substr(pos_col+1, pos_com));

    smap[key] = value;

    buf.erase(0, pos_com + 1);
  }

  free(buf_);

  return shstat_raw{smap["rx_packets"], smap["tx_packets"], smap["rx_bytes"],
	            smap["tx_bytes"], smap["drops"], smap["rx_tcp_out_of_order"]};
}

constexpr uint64_t kNetbenchPort = 8001;
struct payload {
  uint64_t work_iterations;
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t server_queue;
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
  SyntheticWorker *w = workers[core_id];

  if (workn != 0) w->Work(workn);

  // Craft a response.
  ctx->resp_len = sizeof(payload);
  payload *out = reinterpret_cast<payload *>(ctx->resp_buf);
  memcpy(out, in, sizeof(*out));
  out->tsc_end = hton64(rdtscp(&out->cpu));
  out->cpu = hton32(out->cpu);
  out->server_queue = hton64(rt::RuntimeQueueUS());
}

void ServerHandler(void *arg) {
  rt::Thread([] { RPCSStatServer(); }).Detach();
  int num_cores = rt::RuntimeMaxCores();

  for (int i = 0; i < num_cores; ++i) {
    workers[i] = SyntheticWorkerFactory("stridedmem:3200:64");
    if (workers[i] == nullptr) panic("cannot create worker");
  }

  int ret = rt::RpcServerEnable(RpcServer);
  if (ret) panic("couldn't enable RPC server");
  // waits forever.
  rt::WaitGroup(1).Wait();
}

template <class Arrival, class Service>
std::vector<work_unit> GenerateWork(Arrival a, Service s, double cur_us,
                                    double last_us) {
  std::vector<work_unit> w;
  while (true) {
    cur_us += a();
    if (cur_us > last_us) break;
    w.emplace_back(work_unit{cur_us, s(), 0, 0});
  }

  return w;
}

std::vector<work_unit> ClientWorker(
    rt::RpcClient *c, rt::WaitGroup *starter, rt::WaitGroup *starter2,
    std::function<std::vector<work_unit>()> wf) {
  std::vector<work_unit> w(wf());
  std::vector<uint64_t> timings;
  timings.reserve(w.size());

  // Start the receiver thread.
  auto th = rt::Thread([&] {
    payload rp;

    while (true) {
      ssize_t ret = c->Recv(0, &rp, sizeof(rp));
      if (ret != static_cast<ssize_t>(sizeof(rp))) {
        if (ret == 0 || ret < 0) break;
        panic("read failed, ret = %ld", ret);
      }

      uint64_t now = microtime();
      uint64_t idx = ntoh64(rp.index);
      w[idx].duration_us = now - timings[idx];
      w[idx].latency_us = w[idx].duration_us - w[idx].client_queue;
      w[idx].window = c->WinAvail();
      w[idx].tsc = ntoh64(rp.tsc_end);
      w[idx].cpu = ntoh32(rp.cpu);
      w[idx].server_queue = ntoh64(rp.server_queue);
      w[idx].server_time = w[idx].work_us + w[idx].server_queue;
    }
  });

  // Synchronized start of load generation.
  starter->Done();
  starter2->Wait();

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

    timings[i] = microtime();

    // Send an RPC request.
    p.work_iterations = hton64(w[i].work_us * kIterationsPerUS);
    p.index = hton64(i);
    ssize_t ret = c->Send(&p, sizeof(p), &w[i].client_queue);
    if (ret == -ENOBUFS) continue;
    if (ret != static_cast<ssize_t>(sizeof(p)))
      panic("write failed, ret = %ld", ret);
  }

  // rt::Sleep(1 * rt::kSeconds);
  rt::Sleep((int)(kRTT + 2*st));
  BUG_ON(c->Shutdown(SHUT_RDWR));
  th.Join();

  return w;
}

std::vector<work_unit> RunExperiment(
    int threads, struct cstat_raw *csr, struct sstat *ss, double *elapsed,
    std::function<std::vector<work_unit>()> wf) {
  // Create one TCP connection per thread.
  std::vector<std::unique_ptr<rt::RpcClient>> conns;
  sstat_raw s1, s2;
  shstat_raw sh1, sh2;

  for (int i = 0; i < threads; ++i) {
    std::unique_ptr<rt::RpcClient> outc(rt::RpcClient::Dial(raddr, i+1));
    if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
    conns.emplace_back(std::move(outc));
  }

  // Launch a worker thread for each connection.
  rt::WaitGroup starter(threads);
  rt::WaitGroup starter2(1);

  std::vector<rt::Thread> th;
  std::unique_ptr<std::vector<work_unit>> samples[threads];
  for (int i = 0; i < threads; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      auto v = ClientWorker(conns[i].get(), &starter, &starter2, wf);
      samples[i].reset(new std::vector<work_unit>(std::move(v)));
    }));
  }

  if (!b || b->IsLeader()) {
    s1 = ReadRPCSStat();
    sh1 = ReadShenangoStat();
  }

  // Give the workers time to initialize, then start recording.
  starter.Wait();
  if (b && !b->StartExperiment()) {
    exit(0);
  }
  starter2.Done();

  // |--- start experiment duration timing ---|
  barrier();
  timex = std::time(nullptr);
  auto start = steady_clock::now();
  barrier();

  // Wait for the workers to finish.
  for (auto &t : th) t.Join();

  // |--- end experiment duration timing ---|
  barrier();
  auto finish = steady_clock::now();
  barrier();

  if (!b || b->IsLeader()) {
    s2 = ReadRPCSStat();
    sh2 = ReadShenangoStat();
  }

  // Force the connections to close.
  for (auto &c : conns) c->Abort();

  double elapsed_ = duration_cast<sec>(finish - start).count();
  elapsed_ -= kWarmUpTime;

  // Aggregate client stats
  if (csr) {
    for (auto &c : conns) {
      csr->winu_rx += c->StatWinuRx();
      csr->winu_tx += c->StatWinuTx();
      csr->resp_rx += c->StatRespRx();
      csr->req_tx += c->StatReqTx();
      csr->win_expired += c->StatWinExpired();
      csr->req_dropped += c->StatReqDropped();
      c->Close();
    }
  }

  // Aggregate all the samples together.
  std::vector<work_unit> w;
  double min_throughput = 0.0;
  double max_throughput = 0.0;
  uint64_t good_resps = 0;
  uint64_t offered = 0;

  for (int i = 0; i < threads; ++i) {
    auto &v = *samples[i];
    double throughput;
    int slo_success;

    offered += v.size();
    // Remove requests that did not complete.
    v.erase(std::remove_if(v.begin(), v.end(),
			   [](const work_unit &s) {
			   return (s.duration_us == 0 ||
				   s.start_us < kWarmUpTime);}),
	    v.end());
    slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
				return s.duration_us < STRICT_SLO;});
    throughput = static_cast<double>(v.size()) / elapsed_ * 1000000;

    good_resps += slo_success;
    if (i == 0) {
      min_throughput = throughput;
      max_throughput = throughput;
    } else {
      min_throughput = MIN(throughput, min_throughput);
      max_throughput = MAX(throughput, max_throughput);
    }

    w.insert(w.end(), v.begin(), v.end());
  }

  // Report results.
  if (csr) {
    csr->offered_rps = static_cast<double>(offered) / elapsed_ * 1000000;
    csr->offered_rps *= static_cast<double>(kExperimentTime - kWarmUpTime) / kExperimentTime;
    csr->rps = static_cast<double>(w.size()) / elapsed_ * 1000000;
    csr->goodput = static_cast<double>(good_resps) / elapsed_ * 1000000;
    csr->min_percli_tput = min_throughput;
    csr->max_percli_tput = max_throughput;
  }

  if ((!b || b->IsLeader()) && ss) {
    uint64_t idle = s2.idle - s1.idle;
    uint64_t busy = s2.busy - s1.busy;
    ss->cpu_usage = static_cast<double>(busy) / static_cast<double>(idle + busy);

    ss->cpu_usage = (ss->cpu_usage - 1 / static_cast<double>(s1.max_cores)) /
	    (static_cast<double>(s1.num_cores) / static_cast<double>(s1.max_cores));

    uint64_t winu_rx_pkts = s2.winu_rx - s1.winu_rx;
    uint64_t winu_tx_pkts = s2.winu_tx - s1.winu_tx;
    uint64_t win_tx_wins = s2.win_tx - s1.win_tx;
    uint64_t req_rx_pkts = s2.req_rx - s1.req_rx;
    uint64_t req_drop_pkts = s2.req_dropped - s1.req_dropped;
    uint64_t resp_tx_pkts = s2.resp_tx - s1.resp_tx;
    ss->winu_rx_pps = static_cast<double>(winu_rx_pkts) / elapsed_ * 1000000;
    ss->winu_tx_pps = static_cast<double>(winu_tx_pkts) / elapsed_ * 1000000;
    ss->win_tx_wps = static_cast<double>(win_tx_wins) / elapsed_ * 1000000;
    ss->req_rx_pps = static_cast<double>(req_rx_pkts) / elapsed_ * 1000000;
    ss->req_drop_rate = static_cast<double>(req_drop_pkts) / static_cast<double>(req_rx_pkts);
    ss->resp_tx_pps = static_cast<double>(resp_tx_pkts) / elapsed_ * 1000000;

    uint64_t rx_pkts = sh2.rx_pkts - sh1.rx_pkts;
    uint64_t tx_pkts = sh2.tx_pkts - sh1.tx_pkts;
    uint64_t rx_bytes = sh2.rx_bytes - sh1.rx_bytes;
    uint64_t tx_bytes = sh2.tx_bytes - sh1.tx_bytes;
    uint64_t drops = sh2.drops - sh1.drops;
    uint64_t rx_tcp_ooo = sh2.rx_tcp_ooo - sh1.rx_tcp_ooo;
    ss->rx_pps = static_cast<double>(rx_pkts) / elapsed_ * 1000000;
    ss->tx_pps = static_cast<double>(tx_pkts) / elapsed_ * 1000000;
    ss->rx_bps = static_cast<double>(rx_bytes) / elapsed_ * 8000000;
    ss->tx_bps = static_cast<double>(tx_bytes) / elapsed_ * 8000000;
    ss->rx_drops_pps = static_cast<double>(drops) / elapsed_ * 1000000;
    ss->rx_ooo_pps = static_cast<double>(rx_tcp_ooo) / elapsed_ * 1000000;
  }

  *elapsed = elapsed_;

  return w;
}

void PrintHeader(std::ostream& os) {
  os << "num_threads," << "offered_load," << "throughput," << "goodput," << "cpu,"
     << "min," << "mean," << "p50," << "p90," << "p99," << "p999," << "p9999,"
     << "max," << "lmin," << "lmean," << "lp50," << "lp90," << "lp99," << "lp999,"
     << "lp9999," << "lmax," << "p1_win," << "mean_win," << "p99_win," << "p1_q,"
     << "mean_q," << "p99_q," << "mean_stime," << "p99_stime," << "server:rx_pps,"
     << "server:tx_pps," << "server:rx_bps," << "server:tx_bps," << "server:rx_drops_pps,"
     << "server:rx_ooo_pps," << "server:winu_rx_pps," << "server:winu_tx_pps,"
     << "server:win_tx_wps," << "server:req_rx_pps," << "server:req_drop_rate,"
     << "server:resp_tx_pps," << "client:min_tput," << "client:max_tput," << "client:winu_rx_pps,"
     << "client:winu_tx_pps," << "client:resp_rx_pps," << "client:req_tx_pps,"
     << "client:win_expired_wps," << "client:req_dropped_rps" << std::endl;
}

void PrintStatResults(std::vector<work_unit> w, struct cstat *cs,
		      struct sstat *ss) {
  if (w.size() == 0) {
    std::cout << std::setprecision(4) << std::fixed << threads * total_agents
    << "," << cs->offered_rps << "," << "-" << std::endl;
    return;
  }
#if REPORT_TIME_SERIES
  // Sort by start time
  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.start_us < s2.start_us;
  });

  double wallclock = 10000.0;
  std::vector<work_unit> w_;

  for (const work_unit& u : w) {
    if (u.start_us > kWarmUpTime + wallclock) {
      // compute throughput
      double wcount = static_cast<double>(w_.size());
      if (wcount > 0.0) {
        double throughput = wcount / ((double)TIME_SERIES_GRAN / 1000000.0);

        // compute goodput
        double slo_success = static_cast<double>(std::count_if(w_.begin(), w_.end(), [](const work_unit &s) {
          return s.duration_us < STRICT_SLO;
        }));
        double goodput = slo_success / ((double)TIME_SERIES_GRAN / 1000000.0);

        // compute response time
        std::sort(w_.begin(), w_.end(), [](const work_unit &s1, const work_unit &s2) {
          return s1.duration_us < s2.duration_us;
        });
        double p50 = w_[wcount * 0.5].duration_us;
        double p99 = w_[wcount * 0.99].duration_us;

        // Report output
        tseries_out << std::setprecision(4) << std::fixed << wallclock/1000000.0 << ","
          << throughput << "," << goodput << "," << p50 << "," << p99
	  << std::endl;
      } else {
        tseries_out << std::setprecision(4) << std::fixed << wallclock/1000000.0 << ","
          << 0 << "," << 0 << "," << 0 << "," << 0
	  << std::endl;
      }
      // Update Wallclock
      wallclock += (double)TIME_SERIES_GRAN;
      // Clear vector
      w_.clear();
    }
    w_.push_back(u);
  }

  double wcount = static_cast<double>(w_.size());
  if (wcount > 0.0) {
    double throughput = wcount / ((double)TIME_SERIES_GRAN / 1000000.0);

    // compute goodput
    double slo_success = static_cast<double>(std::count_if(w_.begin(), w_.end(), [](const work_unit &s) {
      return s.duration_us < STRICT_SLO;
    }));
    double goodput = slo_success / ((double)TIME_SERIES_GRAN / 1000000.0);

    // compute response time
    std::sort(w_.begin(), w_.end(), [](const work_unit &s1, const work_unit &s2) {
      return s1.duration_us < s2.duration_us;
    });
    double p50 = w_[wcount * 0.5].duration_us;
    double p99 = w_[wcount * 0.99].duration_us;

    // Report output
    tseries_out << std::setprecision(4) << std::fixed << wallclock/1000000.0 << ","
      << throughput << "," << goodput << "," << p50 << "," << p99
      << std::endl;
  }
#endif

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
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

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.latency_us < s2.latency_us;
  });
  double sum_lat = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.latency_us; });
  double mean_lat = sum_lat / w.size();
  double p50_lat = w[count * 0.5].latency_us;
  double p90_lat = w[count * 0.9].latency_us;
  double p99_lat = w[count * 0.99].latency_us;
  double p999_lat = w[count * 0.999].latency_us;
  double p9999_lat = w[count * 0.9999].latency_us;
  double min_lat = w[0].latency_us;
  double max_lat = w[w.size() - 1].latency_us;

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.window < s2.window;
  });
  double sum_win = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.window; });
  double mean_win = sum_win / w.size();
  double p1_win = w[count * 0.01].window;
  double p99_win = w[count * 0.99].window;

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.server_queue < s2.server_queue;
  });
  double sum_que = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.server_queue; });
  double mean_que = sum_que / w.size();
  double p1_que = w[count * 0.01].server_queue;
  double p99_que = w[count * 0.99].server_queue;

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.server_time < s2.server_time;
  });
  double sum_stime = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.server_time; });
  double mean_stime = sum_stime / w.size();
  double p99_stime = w[count * 0.99].server_time;

  double sum_cque = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) {return s + c.client_queue; });
  double mean_cque = sum_cque / w.size();

  std::cout << std::setprecision(4) << std::fixed << threads * total_agents << ","
      << cs->offered_rps << "," << cs->rps << "," << cs->goodput << ","
      << ss->cpu_usage << "," << min << "," << mean << "," << p50 << ","
      << p90 << "," << p99 << "," << p999 << "," << p9999 << "," << max << ","
      << min_lat << "," << mean_lat << "," << p50_lat << "," << p90_lat << ","
      << p99_lat << "," << p999_lat << "," << p9999_lat << "," << max_lat << ","
      << p1_win << "," << mean_win << "," << p99_win << "," << p1_que << ","
      << mean_que << "," << p99_que << "," << mean_stime << ","
      << p99_stime << "," << ss->rx_pps << "," << ss->tx_pps << ","
      << ss->rx_bps << "," << ss->tx_bps << ","
      << ss->rx_drops_pps << "," << ss->rx_ooo_pps << ","
      << ss->winu_rx_pps << "," << ss->winu_tx_pps << ","
      << ss->win_tx_wps << "," << ss->req_rx_pps << ","
      << ss->req_drop_rate << ","<< ss->resp_tx_pps << ","
      << cs->min_percli_tput << "," << cs->max_percli_tput << ","
      << mean_cque << "," << cs->winu_rx_pps << "," << cs->resp_rx_pps << ","
      << cs->req_tx_pps << "," << cs->win_expired_wps << ","
      << cs->req_dropped_rps << std::endl;

  csv_out << std::setprecision(4) << std::fixed << threads * total_agents << ","
      << cs->offered_rps << "," << cs->rps << "," << cs->goodput << ","
      << ss->cpu_usage << "," << min << "," << mean << "," << p50 << ","
      << p90 << "," << p99 << "," << p999 << "," << p9999 << "," << max << ","
      << min_lat << "," << mean_lat << "," << p50_lat << "," << p90_lat << ","
      << p99_lat << "," << p999_lat << "," << p9999_lat << "," << max_lat << ","
      << p1_win << "," << mean_win << "," << p99_win << "," << p1_que << ","
      << mean_que << "," << p99_que << "," << mean_stime << ","
      << p99_stime << "," << ss->rx_pps << "," << ss->tx_pps << ","
      << ss->rx_bps << "," << ss->tx_bps << "," << ss->rx_drops_pps << ","
      << ss->rx_ooo_pps << "," << ss->winu_rx_pps << "," << ss->winu_tx_pps << ","
      << ss->win_tx_wps << "," << ss->req_rx_pps << ","
      << ss->req_drop_rate << "," << ss->resp_tx_pps << ","
      << cs->min_percli_tput << "," << cs->max_percli_tput << ","
      << mean_cque << "," << cs->winu_rx_pps << "," << cs->resp_rx_pps << ","
      << cs->req_tx_pps << "," << cs->win_expired_wps << ","
      << cs->req_dropped_rps << std::endl << std::flush;

  json_out << "{"
	   << "\"num_threads\":" << threads * total_agents << ","
	   << "\"offered_load\":" << cs->offered_rps << ","
	   << "\"throughput\":" << cs->rps << ","
	   << "\"goodput\":" << cs->goodput << ","
	   << "\"cpu\":" << ss->cpu_usage << ","
	   << "\"min\":" << min << ","
	   << "\"mean\":" << mean << ","
	   << "\"p50\":" << p50 << ","
	   << "\"p90\":" << p90 << ","
	   << "\"p99\":" << p99 << ","
	   << "\"p999\":" << p999 << ","
	   << "\"p9999\":" << p9999 << ","
	   << "\"max\":" << max << ","
	   << "\"lmin\":" << min_lat << ","
	   << "\"lmean\":" << mean_lat << ","
	   << "\"lp50\":" << p50_lat << ","
	   << "\"lp90\":" << p90_lat << ","
	   << "\"lp99\":" << p99_lat << ","
	   << "\"lp999\":" << p999_lat << ","
	   << "\"lp9999\":" << p9999_lat << ","
	   << "\"lmax\":" << max_lat << ","
	   << "\"p1_win\":" << p1_win << ","
	   << "\"mean_win\":" << mean_win << ","
	   << "\"p99_win\":" << p99_win << ","
	   << "\"p1_q\":" << p1_que << ","
	   << "\"mean_q\":" << mean_que << ","
	   << "\"p99_q\":" << p99_que << ","
	   << "\"mean_stime\":" << mean_stime << ","
	   << "\"p99_stime\":" << p99_stime << ","
	   << "\"server:rx_pps\":" << ss->rx_pps << ","
	   << "\"server:tx_pps\":" << ss->tx_pps << ","
	   << "\"server:rx_bps\":" << ss->rx_bps << ","
	   << "\"server:tx_bps\":" << ss->tx_bps << ","
	   << "\"server:rx_drops_pps\":" << ss->rx_drops_pps << ","
	   << "\"server:rx_ooo_pps\":" << ss->rx_ooo_pps << ","
	   << "\"server:winu_rx_pps\":" << ss->winu_rx_pps << ","
	   << "\"server:winu_tx_pps\":" << ss->winu_tx_pps << ","
	   << "\"server:win_tx_wps\":" << ss->win_tx_wps << ","
	   << "\"server:req_rx_pps\":" << ss->req_rx_pps << ","
	   << "\"server:req_drop_rate\":" << ss->req_drop_rate << ","
	   << "\"server:resp_tx_pps\":" << ss->resp_tx_pps << ","
	   << "\"client:min_tput\":" << cs->min_percli_tput << ","
	   << "\"client:max_tput\":" << cs->max_percli_tput << ","
	   << "\"client:mean_q\":" << mean_cque << ","
	   << "\"client:winu_rx_pps\":" << cs->winu_rx_pps << ","
	   << "\"client:winu_tx_pps\":" << cs->winu_tx_pps << ","
	   << "\"client:resp_rx_pps\":" << cs->resp_rx_pps << ","
	   << "\"client:req_tx_pps\":" << cs->req_tx_pps << ","
	   << "\"client:win_expired_wps\":" << cs->win_expired_wps << ","
	   << "\"client:req_dropped_rps\":" << cs->req_dropped_rps
	   << "}," << std::endl << std::flush;
}

void SteadyStateExperiment(int threads, double offered_rps,
                           double service_time) {
  struct sstat ss;
  struct cstat_raw csr;
  struct cstat cs;
  double elapsed;

  memset(&csr, 0, sizeof(csr));
  std::vector<work_unit> w = RunExperiment(threads, &csr, &ss, &elapsed,
					   [=] {
    std::mt19937 rg(rand());
    std::mt19937 dg(rand());
    std::exponential_distribution<double> rd(
        1.0 / (1000000.0 / (offered_rps / static_cast<double>(threads))));
    std::exponential_distribution<double> wd(1.0 / service_time);
    return GenerateWork(std::bind(rd, rg), std::bind(wd, dg), 0, kExperimentTime);
  });

  if (b) {
    if (!b->EndExperiment(w, &csr))
      return;
  }

  cs = cstat{csr.offered_rps,
	     csr.rps,
	     csr.goodput,
	     csr.min_percli_tput,
	     csr.max_percli_tput,
	     static_cast<double>(csr.winu_rx) / elapsed * 1000000,
	     static_cast<double>(csr.winu_tx) / elapsed * 1000000,
	     static_cast<double>(csr.resp_rx) / elapsed * 1000000,
	     static_cast<double>(csr.req_tx) / elapsed * 1000000,
	     static_cast<double>(csr.win_expired) / elapsed * 1000000,
	     static_cast<double>(csr.req_dropped) / elapsed * 1000000};

  // Print the results.
  PrintStatResults(w, &cs, &ss);
}

int StringToAddr(const char *str, uint32_t *addr) {
  uint8_t a, b, c, d;

  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;

  *addr = MAKE_IP_ADDR(a, b, c, d);
  return 0;
}

void calculate_rates() {
  if (offered_load > 0.0) {
    offered_loads.push_back(offered_load / (double)total_agents);
  } else {
    if (st == 1.0) {
      offered_loads.push_back(200000 / (double)total_agents);
      offered_loads.push_back(400000 / (double)total_agents);
      offered_loads.push_back(600000 / (double)total_agents);
      offered_loads.push_back(800000 / (double)total_agents);
      offered_loads.push_back(1000000 / (double)total_agents);
      offered_loads.push_back(2000000 / (double)total_agents);
      offered_loads.push_back(3000000 / (double)total_agents);
      offered_loads.push_back(4000000 / (double)total_agents);
      offered_loads.push_back(5000000 / (double)total_agents);
      offered_loads.push_back(6000000 / (double)total_agents);
      offered_loads.push_back(7000000 / (double)total_agents);
      offered_loads.push_back(8000000 / (double)total_agents);
      offered_loads.push_back(9000000 / (double)total_agents);
      offered_loads.push_back(10000000 / (double)total_agents);
      offered_loads.push_back(12000000 / (double)total_agents);
      offered_loads.push_back(16000000 / (double)total_agents);
      offered_loads.push_back(20000000 / (double)total_agents);
    } else if (st == 10.0) {
      offered_loads.push_back(100000 / (double)total_agents);
      offered_loads.push_back(200000 / (double)total_agents);
      offered_loads.push_back(300000 / (double)total_agents);
      offered_loads.push_back(400000 / (double)total_agents);
      offered_loads.push_back(500000 / (double)total_agents);
      offered_loads.push_back(600000 / (double)total_agents);
      offered_loads.push_back(700000 / (double)total_agents);
      offered_loads.push_back(800000 / (double)total_agents);
      offered_loads.push_back(900000 / (double)total_agents);
      offered_loads.push_back(1000000 / (double)total_agents);
      offered_loads.push_back(1100000 / (double)total_agents);
      offered_loads.push_back(1200000 / (double)total_agents);
      offered_loads.push_back(1300000 / (double)total_agents);
      offered_loads.push_back(2000000 / (double)total_agents);
      offered_loads.push_back(3000000 / (double)total_agents);
      offered_loads.push_back(4000000 / (double)total_agents);
    } else if (st == 100.0) {
      offered_loads.push_back(10000 / (double)total_agents);
      offered_loads.push_back(20000 / (double)total_agents);
      offered_loads.push_back(30000 / (double)total_agents);
      offered_loads.push_back(40000 / (double)total_agents);
      offered_loads.push_back(50000 / (double)total_agents);
      offered_loads.push_back(60000 / (double)total_agents);
      offered_loads.push_back(70000 / (double)total_agents);
      offered_loads.push_back(80000 / (double)total_agents);
      offered_loads.push_back(90000 / (double)total_agents);
      offered_loads.push_back(100000 / (double)total_agents);
      offered_loads.push_back(110000 / (double)total_agents);
      offered_loads.push_back(120000 / (double)total_agents);
      offered_loads.push_back(130000 / (double)total_agents);
      offered_loads.push_back(140000 / (double)total_agents);
      offered_loads.push_back(150000 / (double)total_agents);
      offered_loads.push_back(160000 / (double)total_agents);
      offered_loads.push_back(170000 / (double)total_agents);
      offered_loads.push_back(180000 / (double)total_agents);
      offered_loads.push_back(190000 / (double)total_agents);
      offered_loads.push_back(200000 / (double)total_agents);
      offered_loads.push_back(300000 / (double)total_agents);
      offered_loads.push_back(400000 / (double)total_agents);
    }
  }
}

void AgentHandler(void *arg) {
  master.port = kBarrierPort;
  b = new NetBarrier(master);
  BUG_ON(!b);

  calculate_rates();

  for (double i : offered_loads) {
    SteadyStateExperiment(threads, i, st);
  }
}

void ClientHandler(void *arg) {
  int pos;

  if (total_agents > 1) {
    b = new NetBarrier(total_agents - 1);
    BUG_ON(!b);
  }

  calculate_rates();

  std::string json_fname = std::string("outputs/dist_exp_st_") +
	  std::to_string((int)st) + std::string("_nconn_") +
	  std::to_string((int)(threads * total_agents)) + std::string(".json");
  std::string csv_fname = std::string("outputs/dist_exp_st_") +
	  std::to_string((int)st) + std::string("_nconn_") +
	  std::to_string((int)(threads * total_agents)) + std::string(".csv");
  json_out.open(json_fname);
  csv_out.open(csv_fname);
#if REPORT_TIME_SERIES
  std::string tseries_fname = std::string("outputs/tseries_dist_exp_st_") +
	  std::to_string((int)st) + std::string("_nconn_") +
	  std::to_string((int)(threads * total_agents)) + std::string(".csv");
  tseries_out.open(tseries_fname);
#endif
  json_out << "[";

  /* Print Header */
  PrintHeader(csv_out);
  PrintHeader(std::cout);

  for (double i : offered_loads) {
    SteadyStateExperiment(threads, i, st);
    rt::Sleep(1000000);
  }

  pos = json_out.tellp();
  json_out.seekp(pos-2);
  json_out << "]";
  json_out.close();
  csv_out.close();
#if REPORT_TIME_SERIES
  tseries_out.close();
#endif
}

}  // anonymous namespace

int main(int argc, char *argv[]) {
  int ret;

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
  } else if (cmd.compare("agent") == 0) {
    if (argc < 4 || StringToAddr(argv[3], &master.ip)) {
      std::cerr << "usage: [cfg_file] agent [ip_address] ..." << std::endl;
      return -EINVAL;
    }

    if (argc > 4) offered_load = std::stod(argv[4], nullptr);

    ret = runtime_init(argv[1], AgentHandler, NULL);
    if (ret) {
      printf("failed to start runtime\n");
      return ret;
    }
  } else if (cmd.compare("client") != 0) {
    std::cerr << "invalid command: " << cmd << std::endl;
    return -EINVAL;
  }

  if (argc < 6) {
    std::cerr << "usage: [cfg_file] client [#threads] [remote_ip] [service_us] "
                 "[npeers]"
              << std::endl;
    return -EINVAL;
  }

  threads = std::stoi(argv[3], nullptr, 0);

  ret = StringToAddr(argv[4], &raddr.ip);
  if (ret) return -EINVAL;
  raddr.port = kNetbenchPort;

  st = std::stod(argv[5], nullptr);

  if (argc > 6) total_agents += std::stoi(argv[6], nullptr, 0);
  if (argc > 7) offered_load = std::stod(argv[7], nullptr);

  ret = runtime_init(argv[1], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
