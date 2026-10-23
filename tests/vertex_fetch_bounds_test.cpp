// Synthetic microcode only: vertex fetch bounds are per word, before unpacking.
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "../project/src/native_gpu/xenos_hlsl.h"
#include "../project/src/native_gpu/shader_compile.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D failure"); }
static std::string Source(unsigned words, unsigned stride, int offset, bool mini) {
  const unsigned formats[] = {0,36,37,57,38};
  const unsigned swizzle = (1u<<3)|(2u<<6)|(3u<<9);
  std::vector<uint32_t> code{1u|((mini?2u:1u)<<12)|((mini?5u:1u)<<16),0x2000,0};
  auto fetch=[&](bool m, int off) {
    // Full uses fetch constant 0; mini deliberately names constant 1, which
    // must be ignored in favor of the last full fetch's base/size/endianness.
    code.push_back((1u<<12)|(m?(1u<<25):0u));
    code.push_back(swizzle|(formats[words]<<16)|(m?(1u<<30):0u));
    code.push_back(stride|((uint32_t(off)&0x7fffffu)<<8));
  };
  fetch(false,mini?0:offset);
  if(mini) fetch(true,offset);
  bf2n::ShaderInfo info;
  auto source=bf2n::TranslateXenosShader(code.data(),unsigned(code.size()),false,{},info);
  if(!info.ok) throw std::runtime_error(info.error);
  auto at=source.find("return xo;");
  if(at==std::string::npos) throw std::runtime_error("missing vertex return");
  // Expose the actual fetch result, without arithmetic or position conversion.
  source.replace(at,10,"xo.o[0] = r[1]; return xo;");
  source+="\nRWStructuredBuffer<uint4> result:register(u0);\n"
    "[numthreads(1,1,1)] void regressionMain(uint3 id:SV_DispatchThreadID) {"
    "result[0]=asuint(main(xb[0].x).o[0]);}\n";
  return source;
}
int main() {
 try {
  ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
  Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
    D3D11_SDK_VERSION,&dev,nullptr,&ctx));
  // The physical SRV contains nonzero neighbors, so relying on D3D resource
  // bounds instead of the guest fetch descriptor cannot make these tests pass.
  std::array<uint32_t,256> memory{};
  for(unsigned i=0;i<memory.size();++i) memory[i]=std::bit_cast<uint32_t>(float(i)+0.25f);
  D3D11_BUFFER_DESC d{}; d.ByteWidth=sizeof(memory); d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  D3D11_SUBRESOURCE_DATA initial{memory.data(),0,0}; ComPtr<ID3D11Buffer> input;
  Check(dev->CreateBuffer(&d,&initial,&input));
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=DXGI_FORMAT_R32_TYPELESS;
  sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX; sd.BufferEx.NumElements=unsigned(memory.size());
  sd.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
  ComPtr<ID3D11ShaderResourceView> srv; Check(dev->CreateShaderResourceView(input.Get(),&sd,&srv));
  // Translator's global vertex memory binding.
  ctx->CSSetShaderResources(0,1,srv.GetAddressOf());
  d={}; d.ByteWidth=16; d.StructureByteStride=16; d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  ComPtr<ID3D11Buffer> output,readback; Check(dev->CreateBuffer(&d,nullptr,&output));
  d.BindFlags=d.MiscFlags=d.StructureByteStride=0; d.Usage=D3D11_USAGE_STAGING;
  d.CPUAccessFlags=D3D11_CPU_ACCESS_READ; Check(dev->CreateBuffer(&d,nullptr,&readback));
  ComPtr<ID3D11UnorderedAccessView> uav; Check(dev->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
  ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
  unsigned cases=0;
  auto run=[&](unsigned words,unsigned size,unsigned stride,int offset,unsigned index,bool mini,
               bool legacy=false,bool expect_difference=false) {
    auto source=Source(words,stride,offset,mini);
    if(legacy) {
      unsigned removed=0;
      for(auto at=source.find("- vbo < vfl");at!=std::string::npos;at=source.find("- vbo < vfl")) {
        auto begin=source.rfind('\n',at),end=source.find('\n',at);
        source.erase(begin+1,end-begin); ++removed;
      }
      if(!removed) throw std::runtime_error("missing bounds mask for negative control");
    }
    ComPtr<ID3DBlob> code,errors;
    auto hr=D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,"regressionMain",
      "cs_5_0",bf2n::kShaderCompileFlags,0,&code,&errors);
    if(FAILED(hr)&&errors) std::fputs(static_cast<const char*>(errors->GetBufferPointer()),stderr);
    Check(hr); ComPtr<ID3D11ComputeShader> shader;
    Check(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    constexpr unsigned base=128;
    std::array<uint32_t,264> state{}; state[0]=index;
    state[40]=base|3u; state[41]=size<<2;
    state[42]=512|3u; state[43]=200u<<2;
    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth=sizeof(state); cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA ci{state.data(),0,0}; ComPtr<ID3D11Buffer> cb;
    Check(dev->CreateBuffer(&cbd,&ci,&cb)); ctx->CSSetConstantBuffers(1,1,cb.GetAddressOf());
    ctx->CSSetShader(shader.Get(),nullptr,0); ctx->Dispatch(1,1,1);
    ctx->CopyResource(readback.Get(),output.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::array<uint32_t,4> actual{}; std::memcpy(actual.data(),mapped.pData,16); ctx->Unmap(readback.Get(),0);
    std::array<uint32_t,4> expected{};
    for(unsigned component=0;component<words;++component) {
      // Signed reference deliberately independent of shader unsigned subtraction.
      const int64_t relative=int64_t(index)*stride+offset+component;
      if(relative>=0&&relative<size) expected[component]=memory[base/4+relative];
    }
    if((actual!=expected)!=expect_difference) {
      std::fprintf(stderr,"words=%u size=%u stride=%u offset=%d index=%u mini=%u\n",
        words,size,stride,offset,index,unsigned(mini));
      for(unsigned i=0;i<4;++i) std::fprintf(stderr,"%08x != %08x\n",actual[i],expected[i]);
      throw std::runtime_error("vertex fetch differs from per-word bounds reference");
    }
    ++cases;
  };
  for(unsigned words=1;words<=4;++words) {
    for(bool mini:{false,true}) {
      for(unsigned available=0;available<=words;++available) run(words,available,0,0,73,mini);
      run(words,16,words,0,2,mini); // Valid loads must remain bit-identical.
      run(words,4,0,-1,73,mini);   // Begins before base; later words may be valid.
      run(words,4,1,0,4,mini);    // First word at the exclusive end.
      run(words,4,0,3,73,mini);   // Offset creates a partial trailing fetch.
      run(words,4,1,-3,2,mini);   // Indexed fetch straddles the beginning.
      run(words,16,words,0,2,mini,true); // The old valid fetch produces identical bits.
      run(words,0,0,0,73,mini,true,true); // Old OOB fetch must fail the reference.
    }
  }
  std::printf("PASS: %u synthetic vertex fetch bounds cases on WARP, including legacy controls\n",cases);
  return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
