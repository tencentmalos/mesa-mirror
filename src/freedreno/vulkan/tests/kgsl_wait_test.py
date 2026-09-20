#!/usr/bin/env python3
"""Compile production KGSL wait helpers with deterministic clock/ioctl inputs.

Requires only a host C++ compiler. Optional source argument accepts an older
 tu_knl_kgsl.cc to verify the regression counterexample.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'tu_knl_kgsl.cc'
text = source.read_text()
begin = text.index('static inline bool\ntimestamp_cmp(')
end = text.index('static VkResult\nkgsl_queue_wait_fence(', begin)
helpers = text[begin:end]
prelude = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <vector>
#define MIN2(a,b) std::min<uint64_t>(a,b)
using VkResult = int;
constexpr int VK_SUCCESS=0, VK_TIMEOUT=2, VK_ERROR_DEVICE_LOST=-4;
constexpr unsigned KGSL_TIMESTAMP_RETIRED=2;
constexpr unsigned IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID=1;
constexpr unsigned IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID=2;
struct kgsl_cmdstream_readtimestamp_ctxtid { unsigned context_id, type, timestamp; };
struct kgsl_device_waittimestamp_ctxtid { unsigned context_id, timestamp, timeout; };
static uint64_t fake_now=1000000000;
static unsigned reads, waits, retired, checks;
struct Step { int error; uint64_t advance; unsigned timeout; };
static std::vector<Step> steps;
static int read_error;
static uint64_t os_time_get_nano() { return fake_now; }
static int ioctl(int fd, unsigned op, void *arg) {
   assert(fd == 7);
   if (op == IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID) {
      auto *r = static_cast<kgsl_cmdstream_readtimestamp_ctxtid *>(arg);
      assert(r->context_id == 42 && r->type == KGSL_TIMESTAMP_RETIRED);
      ++reads;
      r->timestamp = retired;
      errno = read_error;
      return read_error ? -1 : 0;
   }
   assert(op == IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID);
   auto *w = static_cast<kgsl_device_waittimestamp_ctxtid *>(arg);
   assert(w->context_id == 42);
   // Zero means infinity to KGSL; a Vulkan poll must not use this operation.
   assert(w->timeout != 0);
   assert(waits < steps.size());
   auto s = steps[waits++];
   assert(w->timeout == s.timeout);
   fake_now += s.advance;
   errno = s.error;
   return s.error ? -1 : 0;
}
static int safe_ioctl(int fd, unsigned op, void *arg) { return ioctl(fd, op, arg); }
static void reset() { fake_now=1000000000; reads=waits=read_error=0; retired=9; steps.clear(); }
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
'''
main = r'''
int main() {
   reset();
   CHECK(get_relative_ms(0) == 0);
   CHECK(get_relative_ms(fake_now) == 0);
   CHECK(get_relative_ms(fake_now-1) == 0);
   CHECK(get_relative_ms(fake_now+1) == 1);
   CHECK(get_relative_ms(fake_now+999999) == 1);
   CHECK(get_relative_ms(fake_now+1000000) == 1);
   CHECK(get_relative_ms(fake_now+1000001) == 2);
   CHECK(get_relative_ms(fake_now+(uint64_t(INT_MAX)+9)*1000000) == INT_MAX);
   CHECK(get_relative_ms(UINT64_MAX) == -1);
   CHECK(get_relative_ms(INT64_MAX) == -1);
   for (uint64_t deadline : {uint64_t(0), fake_now-1, fake_now}) {
      CHECK(wait_timestamp_safe(7,42,10,deadline) == VK_TIMEOUT);
      CHECK(waits == 0);
      retired=10;
      CHECK(wait_timestamp_safe(7,42,10,deadline) == VK_SUCCESS);
      retired=11;
      CHECK(wait_timestamp_safe(7,42,10,deadline) == VK_SUCCESS);
      retired=9;
   }
   retired=0;
   CHECK(wait_timestamp_safe(7,42,UINT_MAX,0) == VK_SUCCESS);
   retired=UINT_MAX;
   CHECK(wait_timestamp_safe(7,42,0,0) == VK_TIMEOUT);
   read_error=EINVAL;
   CHECK(wait_timestamp_safe(7,42,0,0) == VK_ERROR_DEVICE_LOST);
   CHECK(waits == 0);
   reset(); steps={{0,0,1}};
   CHECK(wait_timestamp_safe(7,42,10,fake_now+1) == VK_SUCCESS);
   CHECK(reads == 0 && waits == 1);
   reset(); steps={{ETIMEDOUT,1000000,1}};
   CHECK(wait_timestamp_safe(7,42,10,fake_now+999999) == VK_TIMEOUT);
   reset(); steps={{EDEADLK,0,1}};
   CHECK(wait_timestamp_safe(7,42,10,fake_now+1) == VK_ERROR_DEVICE_LOST);
   reset(); steps={{0,0,UINT_MAX}};
   CHECK(wait_timestamp_safe(7,42,10,UINT64_MAX) == VK_SUCCESS);
   reset(); steps={{EINTR,1000000,2},{0,0,1}};
   CHECK(wait_timestamp_safe(7,42,10,fake_now+2000000) == VK_SUCCESS);
   CHECK(waits == 2 && reads == 0);
   reset(); steps={{EAGAIN,1000000,1}};
   CHECK(wait_timestamp_safe(7,42,10,fake_now+1) == VK_TIMEOUT);
   CHECK(waits == 1 && reads == 1);
   reset(); retired=10; steps={{EINTR,1000000,1}};
   CHECK(wait_timestamp_safe(7,42,10,fake_now+1) == VK_SUCCESS);
   CHECK(waits == 1 && reads == 1);
   std::printf("KGSL_WAIT_PASS checks=%u\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='kgsl-wait-test-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(prelude + helpers + main)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wno-narrowing', '-O2', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
