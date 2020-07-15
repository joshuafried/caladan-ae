// rpc.h - support for remote procedure calls (RPCs)

#pragma once

extern "C" {
#include <base/stddef.h>
#include <runtime/rpc.h>
}

#include <functional>

namespace rt {
class RpcClient {
 public:
  // The maximum size of an RPC request payload.
  static constexpr size_t kMaxPayloadSize = SRPC_BUF_SIZE;

  // Disable move and copy.
  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;

  // Creates an RPC session.
  static RpcClient *Dial(netaddr raddr, int id) {
    crpc_session *s;
    raddr.port = SRPC_PORT;
    int ret = crpc_open(raddr, &s, id);
    if (ret) return nullptr;
    return new RpcClient(s);
  }

  int AddConnection(netaddr raddr) {
    raddr.port = SRPC_PORT;
    return crpc_add_conn(raddr, s_);
  }

  // Sends an RPC request.
  ssize_t Send(const void *buf, size_t len) {
    return crpc_send_one(s_, buf, len);
  }

  // Receives an RPC request.
  ssize_t Recv(int conn_id, void *buf, size_t len) {
    return crpc_recv_one(s_, s_->c[conn_id], buf, len);
  }

  uint32_t WinAvail() {
    return crpc_win_avail(s_);
  }

  uint64_t StatWinuRx() {
    return crpc_stat_winu_rx(s_);
  }

  uint64_t StatWinuTx() {
    return crpc_stat_winu_tx(s_);
  }

  uint64_t StatRespRx() {
    return crpc_stat_resp_rx(s_);
  }

  uint64_t StatReqTx() {
    return crpc_stat_req_tx(s_);
  }

  uint64_t StatWinExpired() {
    return crpc_stat_win_expired(s_);
  }

  uint64_t StatReqDropped() {
    return crpc_stat_req_dropped(s_);
  }

  // Shuts down the RPC connection.
  int Shutdown(int how) {
    int i, ret;

    for(i = 0; i < s_->num_conns; ++i) {
      ret = tcp_shutdown(s_->c[i]->c, how);
      if (ret)
        return ret;
    }

    return 0;
  }
  // Aborts the RPC connection.
  void Abort() {
    int i;

    for(i = 0; i < s_->num_conns; ++i)
      tcp_abort(s_->c[i]->c);
  }

  void Close() { crpc_close(s_); }

 private:
  RpcClient(struct crpc_session *s) : s_(s) { }

  // The client session object.
  struct crpc_session *s_;
};

// Enables the RPC server, listening for new sessions.
// Can only be called once.
int RpcServerEnable(std::function<void(struct srpc_ctx *)> f);

uint64_t RpcServerStatWinuRx();
uint64_t RpcServerStatWinuTx();
uint64_t RpcServerStatCreditTx();
uint64_t RpcServerStatReqRx();
uint64_t RpcServerStatReqDropped();
uint64_t RpcServerStatRespTx();
} // namespace rt
