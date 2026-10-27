#pragma once
#include <d3dcompiler.h>

#ifndef _WIN32
// Native SysV ABI blobs for DXVK native. Do not use Wine's COM ABI here.
HRESULT WINAPI NativeD3DCompile(const void* source, SIZE_T size, const char* name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* includes, const char* entry,
    const char* profile, UINT flags, UINT effect_flags, ID3DBlob** code, ID3DBlob** errors);
HRESULT WINAPI NativeD3DCreateBlob(SIZE_T size, ID3DBlob** blob);
#define D3DCompile NativeD3DCompile
#define D3DCreateBlob NativeD3DCreateBlob
#endif
