#include "../project/src/native_gpu/texture_copy.h"
#include "../project/src/native_gpu/cached_state.h"
#include "../project/src/native_gpu/parallel_jobs.h"
#include <array>
#include <bit>
#include <cassert>
#include <cstdio>

int main() {
  std::array<uint8_t,4096> memory{};
  for (size_t i=0;i<memory.size();++i) memory[i]=uint8_t(i*37 + i/7);
  size_t cases=0;
  for (uint32_t sx=0;sx<4;++sx) for(uint32_t address=0;address<64;++address)
    for(uint32_t n=0;n<514;++n) {
      std::array<uint8_t,600> got; got.fill(0xcd);
      bf2n::CopyTextureRow(memory.data(),address,n,sx,got.data()+8);
      for(uint32_t k=0;k<n;++k) assert(got[k+8]==memory[(address+k)^sx]);
      for(uint32_t k=0;k<8;++k) assert(got[k]==0xcd);
      for(uint32_t k=n+8;k<got.size();++k) assert(got[k]==0xcd);
      ++cases;
    }
  bf2n::CachedState<float> value;
  assert(value.Changed(0.f)); assert(!value.Changed(0.f));
  assert(value.Changed(-0.f)); assert(!value.Changed(-0.f));
  float nan=std::bit_cast<float>(0x7fc00001u);
  assert(value.Changed(nan)); assert(!value.Changed(nan));
  assert(value.Changed(std::bit_cast<float>(0x7fc00002u)));
  value={}; assert(value.Changed(nan));
  bf2n::CachedState<void*> ptr;
  assert(ptr.Changed(nullptr)); assert(!ptr.Changed(nullptr));
  ptr={}; assert(ptr.Changed(nullptr));
  {
    bf2n::ParallelJobs jobs;
    for (size_t n : {size_t(0),size_t(1),size_t(31),size_t(32),size_t(2000),size_t(63)}) {
      std::vector<std::atomic<unsigned>> seen(n);
      for(size_t round=0;round<30;++round) {
        for(auto& entry:seen) entry=0;
        jobs.Run(n,[&](size_t i) { ++seen.at(i); });
        for(auto& entry:seen) assert(entry==1);
      }
    }
  }
  std::printf("PASS: %zu endian/alignment/width cases and cache invalidation/signed-zero/NaN cases\n",cases);
}
