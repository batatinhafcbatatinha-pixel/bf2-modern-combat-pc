#pragma once
#include "../platform/native_shader_compiler.h"

namespace bf2n {
// The translator explicitly implements Xenos NaN/Inf and zero-multiply rules.
// FXC otherwise folds isnan(input) to false, even at optimization level 1.
constexpr UINT kShaderCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL1 |
                                     D3DCOMPILE_IEEE_STRICTNESS;
}
