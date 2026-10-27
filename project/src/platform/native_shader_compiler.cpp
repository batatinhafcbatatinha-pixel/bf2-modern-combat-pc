#include "native_shader_compiler.h"
#ifndef _WIN32
#include <vkd3d_shader.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
class Blob final : public ID3DBlob {
 public:
  explicit Blob(size_t size) : data_(size) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (id != __uuidof(IUnknown) && id != __uuidof(ID3D10Blob)) return E_NOINTERFACE;
    *result = static_cast<ID3DBlob*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  void* STDMETHODCALLTYPE GetBufferPointer() override { return data_.data(); }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data_.size(); }
 private:
  std::atomic<ULONG> references_{1};
  std::vector<unsigned char> data_;
};
HRESULT CopyBlob(const void* source, size_t size, ID3DBlob** output) {
  HRESULT result = NativeD3DCreateBlob(size, output);
  if (SUCCEEDED(result) && size) std::memcpy((*output)->GetBufferPointer(), source, size);
  return result;
}
HRESULT Error(const char* message, HRESULT status, ID3DBlob** output) {
  if (output && FAILED(CopyBlob(message, std::strlen(message) + 1, output))) return E_OUTOFMEMORY;
  return status;
}
}

HRESULT WINAPI NativeD3DCreateBlob(SIZE_T size, ID3DBlob** blob) {
  if (!blob) return E_POINTER;
  *blob = nullptr;
  try { *blob = new Blob(size); }
  catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  catch (const std::length_error&) { return E_OUTOFMEMORY; }
  return S_OK;
}

HRESULT WINAPI NativeD3DCompile(const void* source, SIZE_T size, const char* name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* includes, const char* entry,
    const char* profile, UINT flags, UINT effect_flags, ID3DBlob** code, ID3DBlob** errors) {
  if (code) *code = nullptr;
  if (errors) *errors = nullptr;
  if (!source || !size || !profile || !code) return E_INVALIDARG;
  // The game's generated HLSL is self-contained. Reject unsupported callbacks
  // explicitly rather than silently compiling with different include semantics.
  if (includes || effect_flags)
    return Error("Native compiler does not accept include callbacks or effect flags.", E_NOTIMPL, errors);
  try {
    std::vector<vkd3d_shader_macro> macros;
    if (defines) for (; defines->Name; ++defines)
      macros.push_back({defines->Name, defines->Definition});
    vkd3d_shader_preprocess_info preprocess{};
    preprocess.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    preprocess.macros = macros.data();
    preprocess.macro_count = static_cast<unsigned>(macros.size());
    vkd3d_shader_hlsl_source_info hlsl{};
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.next = &preprocess;
    hlsl.entry_point = entry;
    hlsl.profile = profile;
    vkd3d_shader_compile_option packing{
      VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER,
      flags & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR
        ? VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR
        : VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR};
    vkd3d_shader_compile_info info{};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source = {source, size};
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = name;
    info.options = &packing;
    info.option_count = 1;
    // vkd3d-shader's public API has no FXC optimization-level or IEEE-strict
    // switch. Preserve the translator's explicit arithmetic; report this gap.
    if (flags & D3DCOMPILE_IEEE_STRICTNESS) {
      static std::atomic_flag reported = ATOMIC_FLAG_INIT;
      if (!reported.test_and_set()) std::fputs(
          "[shader] vkd3d: FXC IEEE_STRICTNESS has no API mapping; numerical parity is unverified.\n", stderr);
    }
    vkd3d_shader_code result{};
    char* messages = nullptr;
    const int status = vkd3d_shader_compile(&info, &result, &messages);
    HRESULT hr = status >= 0 ? CopyBlob(result.code, result.size, code) : E_FAIL;
    if (errors && messages && *messages) {
      if (FAILED(CopyBlob(messages, std::strlen(messages) + 1, errors))) hr = E_OUTOFMEMORY;
    }
    vkd3d_shader_free_shader_code(&result);
    vkd3d_shader_free_messages(messages);
    if (FAILED(hr) && *code) { (*code)->Release(); *code = nullptr; }
    return hr;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
#endif
