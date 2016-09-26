/* Copyright (c) 2016, Bin Wei <bin@vip.qq.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * The name of of its contributors may not be used to endorse or 
 * promote products derived from this software without specific prior 
 * written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "ccbase/thread.h"
#include "hyperudp/reuse_port_udp_io.h"

namespace hudp {

class ReusePortUdpIO::ThreadContext
{
public:
  ThreadContext(const Env& env, const OnRecv& on_recv)
    : env_(env), on_recv_(on_recv) {}
  ~ThreadContext();
  bool Init(const Addr& listen_addr);
  bool Send(const Buf& buf, const Addr& addr);

private:
  const Env& env_;
  const OnRecv& on_recv_;
  int sock_ = -1;
  std::atomic_bool stop_{true};
  std::thread thread_; 
};

ReusePortUdpIO::ThreadContext::~ThreadContext()
{
  if (!stop_) {
    stop_ = true;
    thread_.join();
  }
}

bool ReusePortUdpIO::ThreadContext::Init(const Addr& listen_addr)
{
  if (sock_ >= 0) {
    ELOG("socket has been initialized!");
    return false;
  }
  if ((sock_ = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
    ELOG("create socket failed (errno: %d) !", errno);
    return false;
  }
  // set reuse-port
  int optval = 1;
  if (setsockopt(sock_, SOL_SOCKET, SO_REUSEPORT,
                          &optval, sizeof(optval)) < 0) {
    ELOG("setsocketopt SO_REUSEPORT failed (errno: %d) !", errno);
    close(sock_);
    sock_ = -1;
    return false;
  }
  // set receive timeout
  struct timeval rto{0, 4000};
  if (setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
                        (void*)&rto, sizeof(rto)) < 0) {
    ELOG("setsocketopt RCVTIMEO failed (errno: %d) !", errno);
    close(sock_);
    sock_ = -1;
    return false;
  }
  // open udp port
  if (bind(sock_, listen_addr.sockaddr_ptr(),
                  listen_addr.sockaddr_len()) < 0) {
    ELOG("bind socket failed (errno: %d) !", errno);
    close(sock_);
    sock_ = -1;
    return false;
  }
  // spawn recv thread
  stop_ = false;
  thread_ = ccb::CreateThread("udp", [this] {
    char buf[65536];
    Addr addr{"0.0.0.0", 0};
    socklen_t addr_len = addr.sockaddr_len();
    while (!stop_) {
      int r = recvfrom(sock_, buf, sizeof(buf), 0, 
                       addr.sockaddr_ptr(), &addr_len);
      if (r > 0 && on_recv_)
        on_recv_({buf, static_cast<size_t>(r)}, addr);
    }
    close(sock_);
    sock_ = -1;
  });
  // init done
  return true;
}

bool ReusePortUdpIO::ThreadContext::Send(const Buf& buf, const Addr& addr)
{
  int r = sendto(sock_, buf.ptr(), buf.len(), MSG_DONTWAIT, 
                 addr.sockaddr_ptr(), addr.sockaddr_len());
  return r >= 0;
}

// class ReusePortUdpIO

std::atomic<size_t> ReusePortUdpIO::cur_thread_index_s{0};

ReusePortUdpIO::ReusePortUdpIO(const Env& env)
  : env_(env)
{
}

ReusePortUdpIO::~ReusePortUdpIO()
{
  // destruct all threads before member destructed
  ctx_vec_.clear();
}

bool ReusePortUdpIO::Init(const Addr& listen_addr, OnRecv on_recv)
{
  on_recv_ = std::move(on_recv);

  size_t thread_num = env_.opt().worker_num;
  for (size_t i = 0; i < thread_num; i++) {
    std::unique_ptr<ThreadContext> ctx{new ThreadContext(env_, on_recv_)};
    if (!ctx->Init(listen_addr)) {
      ctx_vec_.clear();
      return false;
    }
    ctx_vec_.emplace_back(std::move(ctx));
  }
  return true;
}

bool ReusePortUdpIO::Send(const Buf& buf, const Addr& addr)
{
  static thread_local size_t thread_index = cur_thread_index_s++;
  size_t ctx_idx = thread_index % ctx_vec_.size();
  return ctx_vec_[ctx_idx]->Send(buf, addr);
}

} // namespace hudp