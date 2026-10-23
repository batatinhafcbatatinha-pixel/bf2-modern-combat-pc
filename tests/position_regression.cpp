// Game-free regression: equivalent position exports must survive paired swizzles.
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "../project/src/native_gpu/xenos_hlsl.h"
#include "../project/src/native_gpu/shader_compile.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D failure"); }
static std::string Translate(const std::array<unsigned,4>& p, bool constant_first) {
  unsigned sw=0;
  for(unsigned i=0;i<4;++i) sw|=((p[i]-i)&3)<<(i*2);
  // EXEC_END with one position DP4, scalar retain_prev, no game microcode.
  const uint32_t ucode[]={0x1001,0x2000,0,
    62u|(1u<<15)|(1u<<16)|(50u<<26), (sw<<16)|(sw<<8),
    (15u<<24)|(constant_first?(1u<<30):(1u<<31))};
  bf2n::ShaderInfo info;
  auto source=bf2n::TranslateXenosShader(ucode,6,false,{},info);
  if(!info.ok) throw std::runtime_error(info.error);
  // Supply synthetic vertex input through constants, retaining the emitted ALU.
  const std::string original="r[0].x = float(vid);";
  auto at=source.find(original);
  if(at==std::string::npos) throw std::runtime_error("missing input initialization");
  source.replace(at,original.size(),"r[0] = c[1];");
  source+="\nRWStructuredBuffer<float4> result:register(u0);\n"
    "[numthreads(1,1,1)] void regressionMain(uint3 id:SV_DispatchThreadID) {result[0]=main(0).pos;}\n";
  return source;
}
int main() {
 try {
  ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
  Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
    D3D11_SDK_VERSION,&dev,nullptr,&ctx));
  std::array<float,1024> constants{};
  constants[0]=constants[1]=constants[2]=constants[3]=1;
  constants[4]=16777216; constants[5]=1; constants[6]=-16777216; constants[7]=1;
  std::array<uint32_t,264> state{};
  auto sf=[&](unsigned i,float f){state[232+i]=std::bit_cast<uint32_t>(f);};
  sf(0,1); sf(2,1); sf(4,1); sf(6,2); sf(7,2); state[240]=4;
  auto cb=[&](const void* bytes,UINT size) {
    D3D11_BUFFER_DESC d{}; d.ByteWidth=size; d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{bytes,0,0}; ComPtr<ID3D11Buffer> b;
    Check(dev->CreateBuffer(&d,&initial,&b)); return b;
  };
  auto cb0=cb(constants.data(),sizeof(constants)),cb1=cb(state.data(),sizeof(state));
  ctx->CSSetConstantBuffers(0,1,cb0.GetAddressOf()); ctx->CSSetConstantBuffers(1,1,cb1.GetAddressOf());
  D3D11_BUFFER_DESC d{}; d.ByteWidth=16; d.StructureByteStride=16;
  d.BindFlags=D3D11_BIND_UNORDERED_ACCESS; d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  ComPtr<ID3D11Buffer> output,readback; Check(dev->CreateBuffer(&d,nullptr,&output));
  d.BindFlags=d.MiscFlags=d.StructureByteStride=0; d.Usage=D3D11_USAGE_STAGING;
  d.CPUAccessFlags=D3D11_CPU_ACCESS_READ; Check(dev->CreateBuffer(&d,nullptr,&readback));
  ComPtr<ID3D11UnorderedAccessView> uav;
  Check(dev->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
  ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
  auto execute=[&](const std::string& source) {
    ComPtr<ID3DBlob> code,errors;
    auto hr=D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,
      "regressionMain","cs_5_0",bf2n::kShaderCompileFlags,0,&code,&errors);
    if(FAILED(hr)&&errors) std::fputs(static_cast<const char*>(errors->GetBufferPointer()),stderr);
    Check(hr); ComPtr<ID3D11ComputeShader> shader;
    Check(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    ctx->CSSetShader(shader.Get(),nullptr,0); ctx->Dispatch(1,1,1);
    ctx->CopyResource(readback.Get(),output.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::array<uint32_t,4> result{}; std::memcpy(result.data(),mapped.pData,16);
    ctx->Unmap(readback.Get(),0); return result;
  };
  std::array<unsigned,4> p{0,1,2,3};
  auto expected=execute(Translate(p,true));
  if(expected[0]!=std::bit_cast<uint32_t>(1.0f)) throw std::runtime_error("incorrect canonical sum");
  unsigned cases=0;
  do {
    for(bool first:{false,true}) {
      auto source=Translate(p,first);
      if(execute(source)!=expected) throw std::runtime_error("non-invariant position export");
      ++cases;
    }
  } while(std::next_permutation(p.begin(),p.end()));
  // Confirm the cancellation input distinguishes the former accumulation order.
  auto legacy=Translate({0,1,3,2},true);
  auto at=legacy.find("xdot4(v0.xywz, v1.xywz)");
  if(at==std::string::npos) throw std::runtime_error("missing canonical export");
  legacy.replace(at,std::strlen("xdot4(v0.xywz, v1.xywz)"),"xdot4(v0, v1)");
  if(execute(legacy)==expected) throw std::runtime_error("regression input is not discriminating");
  std::printf("PASS: %u translated position permutations are bit-identical on WARP; legacy differs\n",cases);
  return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
