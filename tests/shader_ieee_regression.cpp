// Windows-only, game-free execution of translated arithmetic on D3D11 WARP.
// clang++ -std=c++23 shader_ieee_regression.cpp -ld3d11 -ld3dcompiler -o shader-test.exe
#define NOMINMAX
#include <d3d11.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include "../project/src/native_gpu/shader_compile.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }

static unsigned PixelNaNReadMask(UINT flags) {
  constexpr char pixel[]="float main(float value:TEXCOORD0):SV_Target {return isnan(value)?1.0:0.0;}";
  ComPtr<ID3DBlob> code;
  Check(D3DCompile(pixel,sizeof(pixel)-1,nullptr,nullptr,nullptr,"main","ps_5_0",flags,0,&code,nullptr));
  ComPtr<ID3D11ShaderReflection> reflection;
  Check(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&reflection));
  D3D11_SHADER_DESC desc{}; Check(reflection->GetDesc(&desc));
  for(UINT i=0;i<desc.InputParameters;++i) {
    D3D11_SIGNATURE_PARAMETER_DESC param{}; Check(reflection->GetInputParameterDesc(i,&param));
    if(std::strcmp(param.SemanticName,"TEXCOORD")==0) return param.ReadWriteMask;
  }
  return 0;
}
static std::array<float,12> Execute(UINT flags) {
  constexpr char source[] = R"(
StructuredBuffer<float4> inputs : register(t0);
RWStructuredBuffer<float4> outputs : register(u0);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {
 float4 v=inputs[id.x];
 float zeroMul=(v.y==0.0 || v.z==0.0)?0.0:v.y*v.z;
 float prevMul=(v.x==-3.402823466e+38 || isnan(v.x) || isinf(v.x) ||
   isnan(v.w) || isinf(v.w) || v.w<=0.0)?-3.402823466e+38:v.y*v.x;
 outputs[id.x]=float4(isnan(v.x)?1.0:0.0,zeroMul,prevMul,(v.x>=v.w)?v.x:v.w);
})";
  ComPtr<ID3DBlob> code,errors;
  HRESULT hr=D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",flags,0,&code,&errors);
  if(FAILED(hr) && errors) std::fputs(static_cast<const char*>(errors->GetBufferPointer()),stderr);
  Check(hr);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
  ComPtr<ID3D11ComputeShader> shader;
  Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
  const float inf=std::numeric_limits<float>::infinity(), nan=std::numeric_limits<float>::quiet_NaN();
  const std::array<float,12> input={nan,1,1,1, 1,0,inf,1, inf,1,1,1};
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth=sizeof(input); desc.StructureByteStride=16;
  desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial{input.data(),0,0};
  ComPtr<ID3D11Buffer> src,dst,readback;
  Check(device->CreateBuffer(&desc,&initial,&src));
  desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  Check(device->CreateBuffer(&desc,nullptr,&dst));
  desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;
  desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  Check(device->CreateBuffer(&desc,nullptr,&readback));
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;
  Check(device->CreateShaderResourceView(src.Get(),nullptr,&srv));
  Check(device->CreateUnorderedAccessView(dst.Get(),nullptr,&uav));
  context->CSSetShader(shader.Get(),nullptr,0);
  context->CSSetShaderResources(0,1,srv.GetAddressOf());
  context->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
  context->Dispatch(3,1,1);
  context->CopyResource(readback.Get(),dst.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  Check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
  std::array<float,12> result;
  std::memcpy(result.data(),mapped.pData,sizeof(result));
  context->Unmap(readback.Get(),0);
  return result;
}
int main() {
  try {
    auto fixed=Execute(bf2n::kShaderCompileFlags);
    float max=std::numeric_limits<float>::max(), inf=std::numeric_limits<float>::infinity();
    std::array<float,12> expected={1,1,-max,1, 0,0,0,1, 0,1,-max,inf};
    for(size_t i=0;i<fixed.size();++i) if(fixed[i]!=expected[i]) {
      std::fprintf(stderr,"FAIL index %zu: %.9g expected %.9g\n",i,fixed[i],expected[i]); return 1;
    }
    unsigned oldPixel=PixelNaNReadMask(D3DCOMPILE_OPTIMIZATION_LEVEL1), fixedPixel=PixelNaNReadMask(bf2n::kShaderCompileFlags);
    if(!(fixedPixel & 1)) { std::fputs("FAIL: pixel-input isnan was removed\n",stderr); return 1; }
    std::printf("PASS: WARP NaN/Inf/zero arithmetic; pixel-input isnan read mask old %u, strict %u\n",oldPixel,fixedPixel);
    return 0;
  } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
