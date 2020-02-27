#include "rpc.h"

namespace rt {

namespace {

std::function<void(struct srpc_ctx *)> handler;

void RpcServerTrampoline(struct srpc_ctx *arg) {
  handler(arg);
}

} // namespace

int RpcServerEnable(std::function<void(struct srpc_ctx *)> f) {
  handler = f;
  int ret = srpc_enable(RpcServerTrampoline);
  BUG_ON(ret == -EBUSY);
  return ret;
}

uint64_t RpcServerStatWinupdateSent() {
  return srpc_stat_winupdate_sent();
}

uint64_t RpcServerStatRespSent() {
  return srpc_stat_resp_sent();
}

uint64_t RpcServerStatReqRecvd() {
  return srpc_stat_req_recvd();
}

uint64_t RpcServerStatWinupdateRecvd() {
  return srpc_stat_winupdate_recvd();
}

} // namespace rt
