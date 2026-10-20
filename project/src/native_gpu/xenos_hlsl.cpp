// bf2mc native GPU - Xenos shader microcode -> HLSL translator (see xenos_hlsl.h).
//
// Instruction encodings follow rex/graphics/format/ucode.h (Xenia/XNA documentation). Fields are
// decoded here with explicit shifts so the translator does not depend on bitfield layout.
//
// Control flow: shaders without jumps/calls/loops become straight-line HLSL (cexec = if); the
// rest become a `while (pc != END) switch (pc)` state machine over control flow labels.
//
// ALU semantics notes (from ucode.h): sources of both co-issued operations are read before any
// write; multiplication follows the D3D9 rule 0 * x = 0 (also for inf/NaN); `ps` is the previous
// scalar result; predicated instructions do nothing (also no ps update) when the check fails.

#include "xenos_hlsl.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <sstream>

namespace bf2n {
namespace {

constexpr uint32_t kEnd = 0xFFFFu;

inline uint32_t Bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1u); }
inline int32_t SBits(uint32_t v, uint32_t lo, uint32_t n) {
  return int32_t(v << (32 - lo - n)) >> (32 - n);
}

const char kComp[] = "xyzw";

std::string Fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

std::string FloatLit(float f) {
  if (std::isinf(f)) return f > 0 ? "3.402823466e+38" : "-3.402823466e+38";
  char buf[64];
  snprintf(buf, sizeof(buf), "%.9g", f);
  std::string s = buf;
  if (s.find_first_of(".eE") == std::string::npos) s += ".0";
  return s;
}

// Control flow instruction (48 bits: w0 32 bits, w1 16 bits).
struct Cf {
  uint32_t w0 = 0, w1 = 0;
  uint32_t op() const { return Bits(w1, 12, 4); }
};

enum CfOp : uint32_t {
  kNop = 0, kExec = 1, kExecEnd = 2, kCondExec = 3, kCondExecEnd = 4, kCondExecPred = 5,
  kCondExecPredEnd = 6, kLoopStart = 7, kLoopEnd = 8, kCondCall = 9, kReturn = 10,
  kCondJmp = 11, kAlloc = 12, kCondExecPredClean = 13, kCondExecPredCleanEnd = 14,
  kMarkVsFetchDone = 15,
};

bool IsExec(uint32_t op) {
  return op == kExec || op == kExecEnd || op == kCondExec || op == kCondExecEnd ||
         op == kCondExecPred || op == kCondExecPredEnd || op == kCondExecPredClean ||
         op == kCondExecPredCleanEnd;
}
bool IsExecEnd(uint32_t op) {
  return op == kExecEnd || op == kCondExecEnd || op == kCondExecPredEnd ||
         op == kCondExecPredCleanEnd;
}

class Translator {
 public:
  Translator(const uint32_t* ucode, uint32_t count, bool pixel, const TranslateOptions& opt,
             ShaderInfo& info)
      : ucode_(ucode), count_(count), pixel_(pixel), opt_(opt), info_(info) {}

  std::string Run();

 private:
  // --- decoding helpers
  Cf GetCf(uint32_t index) const {
    uint32_t pair = index >> 1;
    Cf cf;
    if (pair * 3 + 2 >= count_) return cf;
    uint32_t d0 = ucode_[pair * 3], d1 = ucode_[pair * 3 + 1], d2 = ucode_[pair * 3 + 2];
    if (!(index & 1)) {
      cf.w0 = d0;
      cf.w1 = d1 & 0xFFFF;
    } else {
      cf.w0 = (d1 >> 16) | (d2 << 16);
      cf.w1 = d2 >> 16;
    }
    return cf;
  }
  const uint32_t* Instr(uint32_t slot) const {
    static const uint32_t kZero[3] = {};
    if (slot * 3 + 2 >= count_) return kZero;
    return ucode_ + slot * 3;
  }

  void Scan();
  void ScanExec(uint32_t address, uint32_t count, uint32_t sequence);

  // --- emission
  void Line(const std::string& s) {
    out_ << std::string(indent_ * 2, ' ') << s << "\n";
  }
  void EmitCf(uint32_t index, bool state_machine);
  void EmitExec(uint32_t address, uint32_t count, uint32_t sequence);
  void EmitAlu(const uint32_t* w);
  void EmitVfetch(const uint32_t* w);
  void EmitTfetch(const uint32_t* w);
  std::string BoolConst(uint32_t index) const {
    return Fmt("((xb[%u].%c >> %uu) & 1u)", index >> 7, kComp[(index >> 5) & 3], index & 31);
  }
  std::string LoopConst(uint32_t id) const {
    return Fmt("xl[%u].%c", id >> 2, kComp[id & 3]);
  }
  std::string FetchDword(uint32_t dword_index) const {
    return Fmt("xf[%u].%c", dword_index >> 2, kComp[dword_index & 3]);
  }
  std::string Temp(uint32_t index, bool rel) const {
    if (rel) return Fmt("r[(%u + aL) & 63]", index);
    return Fmt("r[%u]", index);
  }
  std::string Exit(bool state_machine) const {
    return state_machine ? "{ pc = 0xFFFFu; break; }" : "break;";
  }
  uint32_t TextureBindingIndex(uint32_t slot, uint32_t dim, const uint32_t* w);

  const uint32_t* ucode_;
  uint32_t count_;
  bool pixel_;
  TranslateOptions opt_;
  ShaderInfo& info_;

  uint32_t cf_count_ = 0;  // number of control flow instructions
  std::set<uint32_t> labels_;
  bool needs_state_machine_ = false;
  bool dynamic_regs_ = false;
  uint32_t max_reg_ = 0;
  std::string err_;

  std::ostringstream out_;
  int indent_ = 1;

  // vfetch_full state reused by vfetch_mini (translation time)
  bool have_full_vfetch_ = false;
};

void Translator::ScanExec(uint32_t address, uint32_t count, uint32_t sequence) {
  for (uint32_t i = 0; i < count; ++i, sequence >>= 2) {
    const uint32_t* w = Instr(address + i);
    if (sequence & 1) {
      // fetch: src 5-10, src rel 11, dst 12-17, dst rel 18
      max_reg_ = std::max({max_reg_, Bits(w[0], 5, 6), Bits(w[0], 12, 6)});
      if (Bits(w[0], 11, 1) || Bits(w[0], 18, 1)) dynamic_regs_ = true;
      uint32_t op = Bits(w[0], 0, 5);
      if (op == 0) {
        uint32_t index = Bits(w[0], 20, 5) * 3 + Bits(w[0], 25, 2);
        info_.vfetch_used[index >> 6] |= uint64_t(1) << (index & 63);
      }
    } else {
      bool exp = Bits(w[0], 15, 1);
      if (!exp) {
        max_reg_ = std::max(max_reg_, Bits(w[0], 0, 6));
        if (Bits(w[0], 6, 1)) dynamic_regs_ = true;
      }
      max_reg_ = std::max(max_reg_, Bits(w[0], 8, 6));
      if (Bits(w[0], 14, 1)) dynamic_regs_ = true;
      for (uint32_t s = 0; s < 3; ++s) {
        bool is_temp = Bits(w[2], 31 - s, 1);
        uint32_t reg = Bits(w[2], 16 - 8 * s, 8);
        if (is_temp) {
          max_reg_ = std::max(max_reg_, reg & 63);
          if (reg & 0x40) dynamic_regs_ = true;
        }
      }
      // mulsc/addsc/subsc temp operand
      uint32_t sop = Bits(w[0], 26, 6);
      if (sop >= 42 && sop <= 47) {
        uint32_t t = (sop & 1) | (Bits(w[2], 29, 1) << 1) | (Bits(w[1], 0, 8) & 0x3C);
        max_reg_ = std::max(max_reg_, t);
      }
    }
  }
}

void Translator::Scan() {
  // The control flow program ends where the first exec's instructions begin.
  uint32_t pair_bound = count_ / 3;
  for (uint32_t i = 0; i < pair_bound; ++i) {
    for (uint32_t j = 0; j < 2; ++j) {
      Cf cf = GetCf(i * 2 + j);
      uint32_t op = cf.op();
      if (IsExec(op)) pair_bound = std::min(pair_bound, Bits(cf.w0, 0, 12));
    }
  }
  cf_count_ = pair_bound * 2;
  labels_.insert(0);
  for (uint32_t i = 0; i < cf_count_; ++i) {
    Cf cf = GetCf(i);
    switch (cf.op()) {
      case kLoopStart:
      case kLoopEnd:
      case kCondJmp:
        labels_.insert(Bits(cf.w0, 0, 13));
        labels_.insert(i + 1);
        needs_state_machine_ = true;
        break;
      case kCondCall:
        labels_.insert(Bits(cf.w0, 0, 13));
        labels_.insert(i + 1);
        needs_state_machine_ = true;
        break;
      case kReturn:
        needs_state_machine_ = true;
        labels_.insert(i + 1);
        break;
      default:
        if (IsExec(cf.op())) ScanExec(Bits(cf.w0, 0, 12), Bits(cf.w0, 12, 3), Bits(cf.w0, 16, 12));
        break;
    }
  }
}

uint32_t Translator::TextureBindingIndex(uint32_t slot, uint32_t dim, const uint32_t* w) {
  TextureBinding b;
  b.slot = slot;
  b.dim = TexDim(dim);
  uint32_t mag = Bits(w[1], 12, 2), min = Bits(w[1], 14, 2), mip = Bits(w[1], 16, 2);
  uint32_t aniso = Bits(w[1], 18, 3);
  b.mag = mag == 3 ? 0xFF : uint8_t(mag);
  b.min = min == 3 ? 0xFF : uint8_t(min);
  b.mip = mip == 3 ? 0xFF : uint8_t(mip);
  b.aniso = aniso == 7 ? 0xFF : uint8_t(aniso);
  for (uint32_t i = 0; i < info_.textures.size(); ++i) {
    const TextureBinding& t = info_.textures[i];
    if (t.slot == b.slot && t.dim == b.dim && t.mag == b.mag && t.min == b.min &&
        t.mip == b.mip && t.aniso == b.aniso) {
      return i;
    }
  }
  info_.textures.push_back(b);
  return uint32_t(info_.textures.size() - 1);
}

// --- ALU ----------------------------------------------------------------------------------------

void Translator::EmitAlu(const uint32_t* w) {
  const uint32_t d0 = w[0], d1 = w[1], d2 = w[2];
  const uint32_t vdest = Bits(d0, 0, 6);
  const bool vdest_rel = Bits(d0, 6, 1);
  const bool abs_const = Bits(d0, 7, 1);
  const uint32_t sdest = Bits(d0, 8, 6);
  const bool sdest_rel = Bits(d0, 14, 1);
  const bool is_export = Bits(d0, 15, 1);
  const uint32_t vmask = Bits(d0, 16, 4);
  const uint32_t smask = Bits(d0, 20, 4);
  const bool vclamp = Bits(d0, 24, 1);
  const bool sclamp = Bits(d0, 25, 1);
  const uint32_t sop = Bits(d0, 26, 6);
  const uint32_t swz[3] = {Bits(d1, 16, 8), Bits(d1, 8, 8), Bits(d1, 0, 8)};
  const bool neg[3] = {bool(Bits(d1, 26, 1)), bool(Bits(d1, 25, 1)), bool(Bits(d1, 24, 1))};
  const bool pred_cond = Bits(d1, 27, 1);
  const bool predicated = Bits(d1, 28, 1);
  const bool const_a0_rel = Bits(d1, 29, 1);
  const bool const_1_rel = Bits(d1, 30, 1);
  const bool const_0_rel = Bits(d1, 31, 1);
  const uint32_t reg[3] = {Bits(d2, 16, 8), Bits(d2, 8, 8), Bits(d2, 0, 8)};
  const uint32_t vop = Bits(d2, 24, 5);
  const bool is_temp[3] = {bool(Bits(d2, 31, 1)), bool(Bits(d2, 30, 1)), bool(Bits(d2, 29, 1))};

  auto const_rel = [&](uint32_t i) -> bool {  // i = 0..2
    if (i == 0) return const_0_rel;
    if (i == 1) return is_temp[0] ? const_0_rel : const_1_rel;
    return (is_temp[0] && is_temp[1]) ? const_0_rel : const_1_rel;
  };
  auto const_ref = [&](uint32_t index, bool rel) -> std::string {
    if (!rel) return Fmt("c[%u]", index);
    if (const_a0_rel) return Fmt("c[(%u + a0) & 255]", index);
    return Fmt("c[(%u + aL) & 255]", index);
  };
  // Full vec4 operand (ALU-relative swizzle).
  auto operand = [&](uint32_t i) -> std::string {
    std::string base;
    bool absolute;
    if (is_temp[i]) {
      base = Temp(reg[i] & 63, reg[i] & 0x40);
      absolute = reg[i] & 0x80;
    } else {
      base = const_ref(reg[i], const_rel(i));
      absolute = abs_const;
    }
    std::string sw;
    for (uint32_t c = 0; c < 4; ++c) sw += kComp[((swz[i] >> (2 * c)) + c) & 3];
    std::string e = base + "." + sw;
    if (absolute) e = "abs(" + e + ")";
    if (neg[i]) e = "-(" + e + ")";
    return e;
  };
  // Single component of operand i (component index c of the ALU swizzle, 3 = `a`, 0 = `b`).
  auto scalar_operand = [&](uint32_t i, uint32_t c) -> std::string {
    std::string base;
    bool absolute;
    if (is_temp[i]) {
      base = Temp(reg[i] & 63, reg[i] & 0x40);
      absolute = reg[i] & 0x80;
    } else {
      base = const_ref(reg[i], const_rel(i));
      absolute = abs_const;
    }
    std::string e = base + "." + kComp[((swz[i] >> (2 * c)) + c) & 3];
    if (absolute) e = "abs(" + e + ")";
    if (neg[i]) e = "-(" + e + ")";
    return e;
  };

  // Operand counts.
  static const uint8_t kVecOperands[32] = {2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 3, 3, 3, 3, 2,
                                           2, 3, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0};
  // scalar: 0 = none, 1 = a, 2 = a b, 3 = const a + temp b
  auto scalar_kind = [](uint32_t op) -> uint32_t {
    switch (op) {
      case 0: case 2: case 4: case 5: case 6: case 23: case 24: case 25:
        return 2;
      case 33: case 50:
        return 0;
      case 42: case 43: case 44: case 45: case 46: case 47:
        return 3;
      default:
        return op <= 49 && op != 41 ? 1 : 0;
    }
  };

  // Export targets.
  enum { kTgtTemp, kTgtInterp, kTgtPos, kTgtPsize, kTgtColor, kTgtDepth, kTgtMem, kTgtNone };
  int target = kTgtTemp;
  uint32_t export_index = 0;
  if (is_export) {
    target = kTgtNone;
    if (vdest == 32 || (vdest >= 33 && vdest <= 37)) {
      target = kTgtMem;
      info_.uses_memexport = true;
    } else if (!pixel_) {
      if (vdest < 16) {
        target = kTgtInterp;
        export_index = vdest;
      } else if (vdest == 62) {
        target = kTgtPos;
      } else if (vdest == 63) {
        target = kTgtPsize;
      }
    } else {
      if (vdest < 4) {
        target = kTgtColor;
        export_index = vdest;
      } else if (vdest == 61) {
        target = kTgtDepth;
      }
    }
  }

  uint32_t vres_mask = vmask, sres_mask = smask, c0_mask = 0, c1_mask = 0;
  if (is_export) {
    vres_mask = vmask & ~smask;
    sres_mask = smask & ~vmask;
    c1_mask = vmask & smask;
    c0_mask = sdest_rel ? (0xF & ~(vmask | smask)) : 0;
  }

  const uint32_t skind = scalar_kind(sop);
  const bool vec_changes_state = (vop >= 20 && vop <= 27) || vop == 29;
  const bool vec_active = vres_mask || vec_changes_state || (is_export && (c0_mask || c1_mask));
  // Every scalar op except retain_prev updates ps; setp/kill/maxas change state.
  const bool scalar_active = sop != 50 || sres_mask;
  if (!vec_active && !scalar_active) return;

  std::string pred_open;
  if (predicated) {
    Line(Fmt("if (p0 == %s) {", pred_cond ? "true" : "false"));
    ++indent_;
  } else {
    Line("{");
    ++indent_;
  }

  // Read all sources first.
  uint32_t nvops = vec_active ? kVecOperands[vop] : 0;
  for (uint32_t i = 0; i < nvops; ++i) Line(Fmt("float4 v%u = %s;", i, operand(i).c_str()));
  if (scalar_active) {
    if (skind == 1) {
      Line(Fmt("float sa = %s;", scalar_operand(2, 3).c_str()));
    } else if (skind == 2) {
      Line(Fmt("float sa = %s;", scalar_operand(2, 3).c_str()));
      Line(Fmt("float sb = %s;", scalar_operand(2, 0).c_str()));
    } else if (skind == 3) {
      // Left: constant (src3 register index, `a` swizzle); right: temp register built from bits.
      uint32_t t = (sop & 1) | (uint32_t(is_temp[2]) << 1) | (swz[2] & 0x3C);
      std::string cst = const_ref(reg[2], const_rel(2)) + "." + kComp[((swz[2] >> 6) + 3) & 3];
      std::string tmp = Temp(t, false) + "." + kComp[(swz[2] + 0) & 3];
      if (abs_const) {
        cst = "abs(" + cst + ")";
        tmp = "abs(" + tmp + ")";
      }
      if (neg[2]) {
        cst = "-(" + cst + ")";
        tmp = "-(" + tmp + ")";
      }
      Line(Fmt("float sa = %s;", cst.c_str()));
      Line(Fmt("float sb = %s;", tmp.c_str()));
    }
  }

  // Vector operation.
  if (vec_active) {
    std::string e;
    switch (vop) {
      case 0: e = "v0 + v1"; break;
      case 1: e = "xmul(v0, v1)"; break;
      case 2: e = "(v0 >= v1) ? v0 : v1"; break;
      case 3: e = "(v0 < v1) ? v0 : v1"; break;
      case 4: e = "float4(v0 == v1)"; break;
      case 5: e = "float4(v0 > v1)"; break;
      case 6: e = "float4(v0 >= v1)"; break;
      case 7: e = "float4(v0 != v1)"; break;
      case 8: e = "v0 - floor(v0)"; break;
      case 9: e = "trunc(v0)"; break;
      case 10: e = "floor(v0)"; break;
      case 11: e = "xmul(v0, v1) + v2"; break;
      case 12: e = "(v0 == 0.0) ? v1 : v2"; break;
      case 13: e = "(v0 >= 0.0) ? v1 : v2"; break;
      case 14: e = "(v0 > 0.0) ? v1 : v2"; break;
      case 15: e = "xdot4(v0, v1).xxxx"; break;
      case 16: e = "xdot3(v0, v1).xxxx"; break;
      case 17: e = "(xmul(v0.x, v1.x) + xmul(v0.y, v1.y) + v2.x).xxxx"; break;
      case 18: e = "xcube(v0, v1)"; break;
      case 19: e = "xmax4(v0).xxxx"; break;
      case 20: case 21: case 22: case 23: {
        static const char* kCmp[] = {"== 0.0", "!= 0.0", "> 0.0", ">= 0.0"};
        const char* cmp = kCmp[vop - 20];
        Line(Fmt("bool vp = (v0.w == 0.0 && v1.w %s);", cmp));
        e = Fmt("((v0.x == 0.0 && v1.x %s) ? 0.0 : v0.x + 1.0).xxxx", cmp);
        break;
      }
      case 24: case 25: case 26: case 27: {
        static const char* kCmp[] = {"==", ">", ">=", "!="};
        const char* cmp = kCmp[vop - 24];
        Line(Fmt("bool vk = any(v0 %s v1);", cmp));
        e = "float4(vk, vk, vk, vk)";
        info_.uses_kill = true;
        break;
      }
      case 28: e = "float4(1.0, xmul(v0.y, v1.y), v0.z, v1.w)"; break;
      case 29:
        Line("int va0 = int(clamp(floor(v0.w + 0.5), -256.0, 255.0));");
        e = "(v0 >= v1) ? v0 : v1";
        break;
      default:
        e = "float4(0.0, 0.0, 0.0, 0.0)";
        break;
    }
    Line("float4 vr = " + e + ";");
    if (vclamp) Line("vr = saturate(vr);");
  }

  // Scalar operation.
  if (scalar_active) {
    std::string e;
    bool sets_p = false, kills = false, sets_a0 = false;
    switch (sop) {
      case 0: e = "sa + sb"; break;
      case 1: e = "sa + ps"; break;
      case 2: e = "xmul(sa, sb)"; break;
      case 3: e = "xmul(sa, ps)"; break;
      case 4:
        e = "(ps == -3.402823466e+38 || isinf(ps) || isnan(ps) || isinf(sb) || isnan(sb) || "
            "sb <= 0.0) ? -3.402823466e+38 : xmul(sa, ps)";
        break;
      case 5: e = "(sa >= sb) ? sa : sb"; break;
      case 6: e = "(sa < sb) ? sa : sb"; break;
      case 7: e = "float(sa == 0.0)"; break;
      case 8: e = "float(sa > 0.0)"; break;
      case 9: e = "float(sa >= 0.0)"; break;
      case 10: e = "float(sa != 0.0)"; break;
      case 11: e = "sa - floor(sa)"; break;
      case 12: e = "trunc(sa)"; break;
      case 13: e = "floor(sa)"; break;
      case 14: e = "exp2(sa)"; break;
      case 15: e = "xlogc(sa)"; break;
      case 16: e = "log2(sa)"; break;
      case 17: e = "xclampmax(1.0 / sa)"; break;
      case 18: e = "xclampzero(1.0 / sa)"; break;
      case 19: e = "1.0 / sa"; break;
      case 20: e = "xclampmax(rsqrt(sa))"; break;
      case 21: e = "xclampzero(rsqrt(sa))"; break;
      case 22: e = "rsqrt(sa)"; break;
      case 23:
        sets_a0 = true;
        Line("int sa0 = int(clamp(floor(sa + 0.5), -256.0, 255.0));");
        e = "(sa >= sb) ? sa : sb";
        break;
      case 24:
        sets_a0 = true;
        Line("int sa0 = int(clamp(floor(sa), -256.0, 255.0));");
        e = "(sa >= sb) ? sa : sb";
        break;
      case 25: e = "sa - sb"; break;
      case 26: e = "sa - ps"; break;
      case 27: sets_p = true; Line("bool sp = (sa == 0.0);"); e = "sp ? 0.0 : 1.0"; break;
      case 28: sets_p = true; Line("bool sp = (sa != 0.0);"); e = "sp ? 0.0 : 1.0"; break;
      case 29: sets_p = true; Line("bool sp = (sa > 0.0);"); e = "sp ? 0.0 : 1.0"; break;
      case 30: sets_p = true; Line("bool sp = (sa >= 0.0);"); e = "sp ? 0.0 : 1.0"; break;
      case 31:
        sets_p = true;
        Line("bool sp = (sa == 1.0);");
        e = "sp ? 0.0 : ((sa == 0.0) ? 1.0 : sa)";
        break;
      case 32:
        sets_p = true;
        Line("bool sp = (sa - 1.0 <= 0.0);");
        e = "sp ? 0.0 : sa - 1.0";
        break;
      case 33: sets_p = true; Line("bool sp = false;"); e = "3.402823466e+38"; break;
      case 34: sets_p = true; Line("bool sp = (sa == 0.0);"); e = "sp ? 0.0 : sa"; break;
      case 35: kills = true; Line("bool sk = (sa == 0.0);"); e = "float(sk)"; break;
      case 36: kills = true; Line("bool sk = (sa > 0.0);"); e = "float(sk)"; break;
      case 37: kills = true; Line("bool sk = (sa >= 0.0);"); e = "float(sk)"; break;
      case 38: kills = true; Line("bool sk = (sa != 0.0);"); e = "float(sk)"; break;
      case 39: kills = true; Line("bool sk = (sa == 1.0);"); e = "float(sk)"; break;
      case 40: e = "sqrt(sa)"; break;
      case 42: case 43: e = "xmul(sa, sb)"; break;
      case 44: case 45: e = "sa + sb"; break;
      case 46: case 47: e = "sa - sb"; break;
      case 48: e = "sin(sa)"; break;
      case 49: e = "cos(sa)"; break;
      case 50: e = "ps"; break;
      default: e = "0.0"; break;
    }
    Line("float sr = " + e + ";");
    if (sclamp) Line("sr = saturate(sr);");
    if (sop != 50) Line("ps = sr;");
    if (sets_p) Line("p0 = sp;");
    if (sets_a0) Line("a0 = sa0;");
    if (kills) {
      info_.uses_kill = true;
      if (pixel_) Line("if (sk) discard;");
    }
  }
  if (vec_active) {
    if (vop >= 20 && vop <= 23) Line("p0 = vp;");
    if (vop == 29) Line("a0 = va0;");
    if (vop >= 24 && vop <= 27 && pixel_) Line("if (vk) discard;");
  }

  // Writes.
  auto mask_str = [](uint32_t m) {
    std::string s;
    for (uint32_t c = 0; c < 4; ++c)
      if (m & (1u << c)) s += kComp[c];
    return s;
  };
  if (!is_export) {
    if (vec_active && vres_mask) {
      std::string m = mask_str(vres_mask);
      Line(Fmt("%s.%s = vr.%s;", Temp(vdest, vdest_rel).c_str(), m.c_str(), m.c_str()));
    }
    if (scalar_active && sres_mask) {
      Line(Fmt("%s.%s = sr;", Temp(sdest, sdest_rel).c_str(), mask_str(sres_mask).c_str()));
    }
  } else if (target != kTgtNone && target != kTgtMem) {
    std::string dst;
    switch (target) {
      case kTgtInterp:
        dst = Fmt("xo.o[%u]", export_index);
        info_.interp_written |= 1u << export_index;
        break;
      case kTgtPos: dst = "xopos"; break;
      case kTgtPsize: dst = "xo.pts"; info_.writes_point_size = true; break;
      case kTgtColor:
        dst = Fmt("oc%u", export_index);
        info_.color_written |= 1u << export_index;
        break;
      case kTgtDepth: dst = "odepth"; info_.writes_depth = true; break;
    }
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t bit = 1u << c;
      if (target == kTgtDepth && c) break;
      std::string lhs = target == kTgtDepth ? dst : dst + "." + kComp[c];
      if (vres_mask & bit) Line(Fmt("%s = vr.%c;", lhs.c_str(), kComp[c]));
      else if (sres_mask & bit) Line(Fmt("%s = sr;", lhs.c_str()));
      else if (c1_mask & bit) Line(Fmt("%s = 1.0;", lhs.c_str()));
      else if (c0_mask & bit) Line(Fmt("%s = 0.0;", lhs.c_str()));
    }
  }

  --indent_;
  Line("}");
}

// --- Vertex fetch -------------------------------------------------------------------------------

void Translator::EmitVfetch(const uint32_t* w) {
  const uint32_t src = Bits(w[0], 5, 6);
  const bool src_rel = Bits(w[0], 11, 1);
  const uint32_t dst = Bits(w[0], 12, 6);
  const bool dst_rel = Bits(w[0], 18, 1);
  const uint32_t fc_index = Bits(w[0], 20, 5) * 3 + Bits(w[0], 25, 2);
  const uint32_t src_comp = Bits(w[0], 30, 2);
  const uint32_t dst_swz = Bits(w[1], 0, 12);
  const bool is_signed = Bits(w[1], 12, 1);
  const bool is_integer = Bits(w[1], 13, 1);  // num_format_all: 0 = normalized
  const uint32_t rf_mode = Bits(w[1], 14, 1);
  const bool rounded = Bits(w[1], 15, 1);
  const uint32_t format = Bits(w[1], 16, 6);
  const int32_t exp_adjust = SBits(w[1], 24, 6);
  const bool mini = Bits(w[1], 30, 1);
  const bool predicated = Bits(w[1], 31, 1);
  const uint32_t stride = Bits(w[2], 0, 8);
  const int32_t offset = SBits(w[2], 8, 23);
  const bool pred_cond = Bits(w[2], 31, 1);

  if (predicated) {
    Line(Fmt("if (p0 == %s) {", pred_cond ? "true" : "false"));
  } else {
    Line("{");
  }
  ++indent_;

  if (!mini) {
    std::string idx = Temp(src, src_rel) + "." + kComp[src_comp];
    std::string fc0 = FetchDword(fc_index * 2);
    if (stride) {
      Line(Fmt("int vfi = int(floor(%s%s));", idx.c_str(), rounded ? " + 0.5" : ""));
      Line(Fmt("vfb = (%s & ~3u) + uint(vfi * %u);", fc0.c_str(), stride * 4));
    } else {
      Line(Fmt("vfb = %s & ~3u;", fc0.c_str()));
    }
    Line(Fmt("vfe = %s & 3u;", FetchDword(fc_index * 2 + 1).c_str()));
    have_full_vfetch_ = true;
  }

  // Words per element.
  uint32_t words;
  switch (format) {
    case 26: case 32: case 34: case 37: words = 2; break;  // 16x4, 16x4f, 32x2, 32x2f
    case 57: words = 3; break;                            // 32x3f
    case 35: case 38: words = 4; break;                   // 32x4, 32x4f
    default: words = 1; break;
  }
  Line(Fmt("uint vfa = vfb + uint(%d);", offset * 4));
  if (words == 1) Line("uint4 vw = uint4(xvmem.Load(vfa), 0, 0, 0);");
  else if (words == 2) Line("uint4 vw = uint4(xvmem.Load2(vfa), 0, 0);");
  else if (words == 3) Line("uint4 vw = uint4(xvmem.Load3(vfa), 0);");
  else Line("uint4 vw = xvmem.Load4(vfa);");
  Line("vw = xswap(vw, vfe);");

  // Unpack.
  int widths[4] = {0, 0, 0, 0}, offs[4] = {0, 0, 0, 0}, word_of[4] = {0, 0, 0, 0};
  int comps = 0;
  bool packed = true;
  switch (format) {
    case 6: comps = 4; widths[0] = widths[1] = widths[2] = widths[3] = 8;
      offs[1] = 8; offs[2] = 16; offs[3] = 24; break;
    case 7: comps = 4; widths[0] = widths[1] = widths[2] = 10; widths[3] = 2;
      offs[1] = 10; offs[2] = 20; offs[3] = 30; break;
    case 16: comps = 3; widths[0] = widths[1] = 11; widths[2] = 10;
      offs[1] = 11; offs[2] = 22; break;
    case 17: comps = 3; widths[0] = 10; widths[1] = widths[2] = 11;
      offs[1] = 10; offs[2] = 21; break;
    case 25: comps = 2; widths[0] = widths[1] = 16; offs[1] = 16; break;
    case 26: comps = 4; widths[0] = widths[1] = widths[2] = widths[3] = 16;
      offs[1] = offs[3] = 16; word_of[2] = word_of[3] = 1; break;
    default: packed = false; break;
  }
  Line("float4 vv = float4(0.0, 0.0, 0.0, 0.0);");
  if (packed) {
    for (int c = 0; c < comps; ++c) {
      int wd = widths[c], of = offs[c];
      std::string word = Fmt("vw.%c", kComp[word_of[c]]);
      if (is_signed) {
        Line(Fmt("vv.%c = float(int(%s << %du) >> %d);", kComp[c], word.c_str(), 32 - of - wd,
                 32 - wd));
        if (!is_integer) {
          if (rf_mode == 0) {
            if (wd > 2) Line(Fmt("vv.%c *= %s;", kComp[c], FloatLit(1.0f / float((1u << (wd - 1)) - 1)).c_str()));
            Line(Fmt("vv.%c = max(vv.%c, -1.0);", kComp[c], kComp[c]));
          } else {
            float z = 1.0f / float((1u << wd) - 1);
            Line(Fmt("vv.%c = vv.%c * %s + %s;", kComp[c], kComp[c], FloatLit(2.0f * z).c_str(),
                     FloatLit(z).c_str()));
          }
        }
      } else {
        Line(Fmt("vv.%c = float((%s >> %du) & %uu);", kComp[c], word.c_str(), of,
                 (wd == 32) ? 0xFFFFFFFFu : ((1u << wd) - 1)));
        if (!is_integer && wd > 1) {
          Line(Fmt("vv.%c *= %s;", kComp[c], FloatLit(1.0f / float((1u << wd) - 1)).c_str()));
        }
      }
    }
  } else {
    switch (format) {
      case 31:  // 16_16_FLOAT
        Line("vv.xy = f16tof32(uint2(vw.x, vw.x >> 16));");
        break;
      case 32:  // 16_16_16_16_FLOAT
        Line("vv = f16tof32(uint4(vw.x, vw.x >> 16, vw.y, vw.y >> 16));");
        break;
      case 33: case 34: case 35: {  // 32 / 32_32 / 32x4 integer
        int n = format == 33 ? 1 : format == 34 ? 2 : 4;
        std::string m = std::string("xyzw").substr(0, n);
        if (is_signed) Line(Fmt("vv.%s = float%d(asint(vw.%s));", m.c_str(), n, m.c_str()));
        else Line(Fmt("vv.%s = float%d(vw.%s);", m.c_str(), n, m.c_str()));
        if (!is_integer) {
          if (is_signed) {
            if (rf_mode == 0) Line(Fmt("vv.%s *= %s;", m.c_str(), FloatLit(1.0f / 2147483647.0f).c_str()));
            else Line(Fmt("vv.%s = vv.%s * %s + %s;", m.c_str(), m.c_str(),
                          FloatLit(2.0f / 4294967295.0f).c_str(), FloatLit(1.0f / 4294967295.0f).c_str()));
          } else {
            Line(Fmt("vv.%s *= %s;", m.c_str(), FloatLit(1.0f / 4294967295.0f).c_str()));
          }
        }
        break;
      }
      case 36: Line("vv.x = asfloat(vw.x);"); break;
      case 37: Line("vv.xy = asfloat(vw.xy);"); break;
      case 38: Line("vv = asfloat(vw);"); break;
      case 57: Line("vv.xyz = asfloat(vw.xyz);"); break;
      default: break;
    }
  }
  if (exp_adjust) Line(Fmt("vv *= %s;", FloatLit(std::ldexp(1.0f, exp_adjust)).c_str()));

  // Destination swizzle.
  std::string d = Temp(dst, dst_rel);
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t s = (dst_swz >> (3 * c)) & 7;
    if (s == 7) continue;
    if (s < 4) Line(Fmt("%s.%c = vv.%c;", d.c_str(), kComp[c], kComp[s]));
    else Line(Fmt("%s.%c = %s;", d.c_str(), kComp[c], s == 5 ? "1.0" : "0.0"));
  }
  --indent_;
  Line("}");
}

// --- Texture fetch ------------------------------------------------------------------------------

void Translator::EmitTfetch(const uint32_t* w) {
  const uint32_t op = Bits(w[0], 0, 5);
  const uint32_t src = Bits(w[0], 5, 6);
  const bool src_rel = Bits(w[0], 11, 1);
  const uint32_t dst = Bits(w[0], 12, 6);
  const bool dst_rel = Bits(w[0], 18, 1);
  const uint32_t slot = Bits(w[0], 20, 5);
  const bool denorm = Bits(w[0], 25, 1);
  const uint32_t src_swz = Bits(w[0], 26, 6);
  const uint32_t dst_swz = Bits(w[1], 0, 12);
  const bool use_comp_lod = Bits(w[1], 28, 1);
  const bool use_reg_lod = Bits(w[1], 29, 1);
  const bool predicated = Bits(w[1], 31, 1);
  const bool use_reg_grad = Bits(w[2], 0, 1);
  const int32_t lod_bias = SBits(w[2], 2, 7);
  const uint32_t dim = Bits(w[2], 14, 2);
  const int32_t off[3] = {SBits(w[2], 16, 5), SBits(w[2], 21, 5), SBits(w[2], 26, 5)};
  const bool pred_cond = Bits(w[2], 31, 1);

  if (predicated) {
    Line(Fmt("if (p0 == %s) {", pred_cond ? "true" : "false"));
  } else {
    Line("{");
  }
  ++indent_;

  std::string s = Temp(src, src_rel);
  std::string coord = Fmt("float3(%s.%c, %s.%c, %s.%c)", s.c_str(), kComp[src_swz & 3], s.c_str(),
                          kComp[(src_swz >> 2) & 3], s.c_str(), kComp[(src_swz >> 4) & 3]);

  switch (op) {
    case 24:  // setTexLOD
      Line(Fmt("xlod = %s.%c;", s.c_str(), kComp[src_swz & 3]));
      --indent_;
      Line("}");
      return;
    case 25:  // setGradientH
      Line(Fmt("xgh = %s.xyz;", coord.c_str()));
      --indent_;
      Line("}");
      return;
    case 26:  // setGradientV
      Line(Fmt("xgv = %s.xyz;", coord.c_str()));
      --indent_;
      Line("}");
      return;
    default:
      break;
  }

  if (op == 18) {  // getGradients: XZ = ddx(src.xy), YW = ddy(src.xy)
    if (pixel_) {
      Line(Fmt("float2 tg = %s.xy;", coord.c_str()));
      Line("float4 tr = float4(ddx(tg).x, ddy(tg).x, ddx(tg).y, ddy(tg).y);");
    } else {
      Line("float4 tr = float4(0.0, 0.0, 0.0, 0.0);");
    }
  } else {
    uint32_t b = TextureBindingIndex(slot, dim, w);
    std::string tex = Fmt("xt%u", b), smp = Fmt("xs%u", b);
    Line(Fmt("float3 tc = %s;", coord.c_str()));
    // Texture size for unnormalized coordinates / offsets / weights.
    bool need_size = denorm || off[0] || off[1] || off[2] || op == 19;
    if (need_size) {
      if (dim == 2) {
        Line(Fmt("float3 tsz; %s.GetDimensions(tsz.x, tsz.y, tsz.z);", tex.c_str()));
      } else if (dim == 3) {
        Line(Fmt("float2 tsz2; %s.GetDimensions(tsz2.x, tsz2.y); float3 tsz = float3(tsz2, 1.0);",
                 tex.c_str()));
      } else {
        Line(Fmt("float3 tsa; %s.GetDimensions(tsa.x, tsa.y, tsa.z); float3 tsz = float3(tsa.xy, 1.0);",
                 tex.c_str()));
      }
      if (denorm && dim != 3) Line("tc.xy /= tsz.xy;" );
      if (denorm && dim == 2) Line("tc.z /= tsz.z;");
      if (off[0] || off[1] || off[2]) {
        Line(Fmt("tc += float3(%s, %s, %s) / tsz;", FloatLit(off[0] * 0.5f).c_str(),
                 FloatLit(off[1] * 0.5f).c_str(), FloatLit(off[2] * 0.5f).c_str()));
      }
    }
    // Coordinates in the host resource's space.
    std::string hc;
    switch (dim) {
      case 0: hc = "float3(tc.x, 0.5, 0.0)"; break;
      case 1: hc = "float3(tc.xy, 0.0)"; break;
      case 2: hc = "tc"; break;
      default: hc = "xcubedir(tc)"; break;
    }
    float bias = lod_bias / 16.0f;
    if (op == 1) {
      // grad3: gradients are float3 (3D / cube), else float2.
      auto make_sample = [&](const std::string& t, const std::string& c, bool grad3) {
        if (use_reg_grad) {
          return grad3 ? Fmt("%s.SampleGrad(%s, %s, xgh, xgv)", t.c_str(), smp.c_str(), c.c_str())
                       : Fmt("%s.SampleGrad(%s, %s, xgh.xy, xgv.xy)", t.c_str(), smp.c_str(), c.c_str());
        }
        if (pixel_ && use_comp_lod && !use_reg_lod) {
          if (bias != 0.0f) {
            return Fmt("%s.SampleBias(%s, %s, %s)", t.c_str(), smp.c_str(), c.c_str(), FloatLit(bias).c_str());
          }
          return Fmt("%s.Sample(%s, %s)", t.c_str(), smp.c_str(), c.c_str());
        }
        std::string lod = use_reg_lod ? "xlod" : "0.0";
        if (bias != 0.0f) lod += " + " + FloatLit(bias);
        return Fmt("%s.SampleLevel(%s, %s, %s)", t.c_str(), smp.c_str(), c.c_str(), lod.c_str());
      };
      if (dim == 2) {
        // "3D or stacked": the fetch constant decides (dimension 2 = 3D, else 2D array whose layer
        // is the normalized z).
        Line(Fmt("float3 tas; %sa.GetDimensions(tas.x, tas.y, tas.z);", tex.c_str()));
        Line("float4 tr3 = " + make_sample(tex, "tc", true) + ";");
        Line("float4 tr2 = " + make_sample(tex + "a", "float3(tc.xy, tc.z * tas.z - 0.5)", false) + ";");
        Line(Fmt("float4 tr = (((%s >> 9) & 3u) == 2u) ? tr3 : tr2;",
                 FetchDword(slot * 6 + 5).c_str()));
      } else {
        Line("float4 tr = " + make_sample(tex, hc, dim == 3) + ";");
      }
      Line(Fmt("tr = xtexpost(tr, %s, %s, %s);", FetchDword(slot * 6 + 0).c_str(),
               FetchDword(slot * 6 + 1).c_str(), FetchDword(slot * 6 + 3).c_str()));
    } else if (op == 17) {  // getCompTexLOD
      if (pixel_) {
        Line(Fmt("float4 tr = float4(%s.CalculateLevelOfDetailUnclamped(%s, %s), 0.0, 0.0, 0.0);",
                 tex.c_str(), smp.c_str(), dim == 2 ? "tc" : dim == 3 ? "xcubedir(tc)" : "tc.xy"));
      } else {
        Line("float4 tr = float4(0.0, 0.0, 0.0, 0.0);");
      }
    } else if (op == 19) {  // getWeights
      Line("float4 tr = float4(frac(tc * tsz - 0.5), 0.0);");
    } else {  // getBCF and unknown
      Line("float4 tr = float4(0.0, 0.0, 0.0, 0.0);");
    }
  }

  std::string d = Temp(dst, dst_rel);
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t sw = (dst_swz >> (3 * c)) & 7;
    if (sw == 7) continue;
    if (sw < 4) Line(Fmt("%s.%c = tr.%c;", d.c_str(), kComp[c], kComp[sw]));
    else Line(Fmt("%s.%c = %s;", d.c_str(), kComp[c], sw == 5 ? "1.0" : "0.0"));
  }
  --indent_;
  Line("}");
}

// --- Control flow -------------------------------------------------------------------------------

void Translator::EmitExec(uint32_t address, uint32_t count, uint32_t sequence) {
  for (uint32_t i = 0; i < count; ++i, sequence >>= 2) {
    const uint32_t* w = Instr(address + i);
    if (sequence & 1) {
      if (Bits(w[0], 0, 5) == 0) EmitVfetch(w);
      else EmitTfetch(w);
    } else {
      EmitAlu(w);
    }
  }
}

void Translator::EmitCf(uint32_t index, bool sm) {
  Cf cf = GetCf(index);
  uint32_t op = cf.op();
  switch (op) {
    case kNop:
    case kAlloc:
    case kMarkVsFetchDone:
      return;
    case kExec:
    case kExecEnd:
      Line(Fmt("// %u: exec%s", index, op == kExecEnd ? "e" : ""));
      EmitExec(Bits(cf.w0, 0, 12), Bits(cf.w0, 12, 3), Bits(cf.w0, 16, 12));
      if (op == kExecEnd) Line(Exit(sm));
      return;
    case kCondExec:
    case kCondExecEnd:
    case kCondExecPredClean:
    case kCondExecPredCleanEnd: {
      uint32_t b = Bits(cf.w1, 2, 8);
      bool cond = Bits(cf.w1, 10, 1);
      Line(Fmt("// %u: cexec b%u == %d", index, b, cond));
      Line(Fmt("if (%s == %uu) {", BoolConst(b).c_str(), cond ? 1u : 0u));
      ++indent_;
      EmitExec(Bits(cf.w0, 0, 12), Bits(cf.w0, 12, 3), Bits(cf.w0, 16, 12));
      if (IsExecEnd(op)) Line(Exit(sm));
      --indent_;
      Line("}");
      return;
    }
    case kCondExecPred:
    case kCondExecPredEnd: {
      bool cond = Bits(cf.w1, 10, 1);
      Line(Fmt("// %u: (p0 == %d) exec", index, cond));
      Line(Fmt("if (p0 == %s) {", cond ? "true" : "false"));
      ++indent_;
      EmitExec(Bits(cf.w0, 0, 12), Bits(cf.w0, 12, 3), Bits(cf.w0, 16, 12));
      if (IsExecEnd(op)) Line(Exit(sm));
      --indent_;
      Line("}");
      return;
    }
    case kLoopStart: {
      uint32_t id = Bits(cf.w0, 16, 5);
      bool repeat = Bits(cf.w0, 13, 1);
      uint32_t skip = Bits(cf.w0, 0, 13);
      Line(Fmt("// %u: loop i%u", index, id));
      Line("lst[lsp & 3] = aL; lit[lsp & 3] = lc; lsp++;");
      Line(Fmt("lc = 0; %s", repeat ? "" : Fmt("aL = int((%s >> 8) & 0xFFu);", LoopConst(id).c_str()).c_str()));
      Line(Fmt("if ((%s & 0xFFu) == 0u) { lsp--; aL = lst[lsp & 3]; lc = lit[lsp & 3]; pc = %uu; break; }",
               LoopConst(id).c_str(), skip));
      return;
    }
    case kLoopEnd: {
      uint32_t id = Bits(cf.w0, 16, 5);
      bool pbreak = Bits(cf.w0, 21, 1);
      bool cond = Bits(cf.w1, 10, 1);
      uint32_t body = Bits(cf.w0, 0, 13);
      Line(Fmt("// %u: endloop i%u", index, id));
      Line(Fmt("lc++; aL += int(%s << 8) >> 24;", LoopConst(id).c_str()));
      std::string brk = pbreak ? Fmt(" && !(p0 == %s)", cond ? "true" : "false") : "";
      Line(Fmt("if (lc < int(%s & 0xFFu)%s) { pc = %uu; break; }", LoopConst(id).c_str(),
               brk.c_str(), body));
      Line("lsp--; aL = lst[lsp & 3]; lc = lit[lsp & 3];");
      return;
    }
    case kCondCall: {
      uint32_t target = Bits(cf.w0, 0, 13);
      bool uncond = Bits(cf.w0, 13, 1);
      bool predc = Bits(cf.w0, 14, 1);
      bool cond = Bits(cf.w1, 10, 1);
      std::string c = uncond ? "true"
                             : predc ? Fmt("p0 == %s", cond ? "true" : "false")
                                     : Fmt("%s == %uu", BoolConst(Bits(cf.w1, 2, 8)).c_str(), cond ? 1u : 0u);
      Line(Fmt("// %u: call %u", index, target));
      Line(Fmt("if (%s) { cst[csp & 3] = %uu; csp++; pc = %uu; break; }", c.c_str(), index + 1, target));
      return;
    }
    case kReturn:
      Line(Fmt("// %u: ret", index));
      Line("if (csp > 0) { csp--; pc = cst[csp & 3]; break; }");
      return;
    case kCondJmp: {
      uint32_t target = Bits(cf.w0, 0, 13);
      bool uncond = Bits(cf.w0, 13, 1);
      bool predc = Bits(cf.w0, 14, 1);
      bool cond = Bits(cf.w1, 10, 1);
      std::string c = uncond ? "true"
                             : predc ? Fmt("p0 == %s", cond ? "true" : "false")
                                     : Fmt("%s == %uu", BoolConst(Bits(cf.w1, 2, 8)).c_str(), cond ? 1u : 0u);
      Line(Fmt("// %u: jmp %u", index, target));
      Line(Fmt("if (%s) { pc = %uu; break; }", c.c_str(), target));
      return;
    }
    default:
      return;
  }
}

// --- Prologue / epilogue ------------------------------------------------------------------------

const char kCommonHlsl[] = R"(
cbuffer XConsts : register(b0) { float4 c[256]; };
cbuffer XState : register(b1) {
  uint4 xb[2];
  uint4 xl[8];
  uint4 xf[48];
  float4 xsys[8];
};
struct XVSOut {
  float4 pos : SV_Position;
  float4 o[16] : TEXCOORD0;
  float4 pts : TEXCOORD16;
};
float xmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 xmul(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float xdot4(float4 a, float4 b) { float4 m = xmul(a, b); return m.x + m.y + m.z + m.w; }
float xdot3(float4 a, float4 b) { float4 m = xmul(a, b); return m.x + m.y + m.z; }
float xmax4(float4 a) {
  if (a.x >= a.y && a.x >= a.z && a.x >= a.w) return a.x;
  if (a.y >= a.z && a.y >= a.w) return a.y;
  if (a.z >= a.w) return a.z;
  return a.w;
}
float xlogc(float a) { float t = log2(a); return isinf(t) && t < 0.0 ? -3.402823466e+38 : t; }
float xclampmax(float t) { return isinf(t) ? (t < 0.0 ? -3.402823466e+38 : 3.402823466e+38) : t; }
float xclampzero(float t) { return isinf(t) ? (t < 0.0 ? -0.0 : 0.0) : t; }
float4 xcube(float4 s0, float4 s1) {
  float3 v = float3(s0.z, s0.w, s0.x);
  float3 a = abs(v);
  float tc, sc, ma, id;
  if (a.z >= a.x && a.z >= a.y) {
    tc = -v.y; sc = v.z < 0.0 ? -v.x : v.x; ma = 2.0 * v.z; id = v.z < 0.0 ? 5.0 : 4.0;
  } else if (a.y >= a.x) {
    tc = v.y < 0.0 ? -v.z : v.z; sc = v.x; ma = 2.0 * v.y; id = v.y < 0.0 ? 3.0 : 2.0;
  } else {
    tc = -v.y; sc = v.x < 0.0 ? v.z : -v.z; ma = 2.0 * v.x; id = v.x < 0.0 ? 1.0 : 0.0;
  }
  return float4(tc, sc, ma, id);
}
float3 xcubedir(float3 t) {
  float u = t.x * 2.0 - 3.0, v = t.y * 2.0 - 3.0;
  int f = int(t.z + 0.5);
  if (f == 0) return float3(1.0, -v, -u);
  if (f == 1) return float3(-1.0, -v, u);
  if (f == 2) return float3(u, 1.0, v);
  if (f == 3) return float3(u, -1.0, -v);
  if (f == 4) return float3(u, -v, 1.0);
  return float3(-u, -v, -1.0);
}
uint4 xswap(uint4 v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2u || e == 3u) v = (v << 16) | (v >> 16);
  return v;
}
float xdegamma(float v) {
  // Xenos piecewise-linear gamma to linear.
  float s = saturate(v) * 255.0;
  float l = s < 64.0 ? s : (s < 96.0 ? 64.0 + (s - 64.0) * 2.0
            : (s < 192.0 ? 128.0 + (s - 96.0) * 4.0 : 512.0 + (s - 192.0) * 8.0));
  return l / 1023.0;
}
float4 xtexpost(float4 t, uint d0, uint d1, uint d3) {
  // Components a format lacks read as copies of its last one (RRRR / RGGG / RGBB), as on Xenos
  // (table from Xenia's texture cache), before the sign and the fetch swizzle apply.
  uint fmt = d1 & 63u;
  uint bit = 1u << (fmt & 31u);
  bool hi = fmt >= 32u;
  if (((hi ? 0x0C00DA92u : 0x49C00307u) & bit) != 0u) t = t.rrrr;
  else if (((hi ? 0x10032524u : 0x92002400u) & bit) != 0u) t = t.rggg;
  else if (((hi ? 0x03800000u : 0x00031830u) & bit) != 0u) t = t.rgbb;
  // Sign: 0 unsigned, 1 signed (host format), 2 unsigned biased, 3 gamma.
  [unroll] for (int i = 0; i < 4; ++i) {
    uint sg = (d0 >> (2 + 2 * i)) & 3u;
    if (sg == 2u) t[i] = t[i] * 2.0 - 1.0;
    else if (sg == 3u) t[i] = xdegamma(t[i]);
  }
  float4 o;
  [unroll] for (int j = 0; j < 4; ++j) {
    uint s = (d3 >> (1 + 3 * j)) & 7u;
    o[j] = s < 4u ? t[s] : (s == 5u ? 1.0 : 0.0);
  }
  int e = int(d3 << 13) >> 26;
  if (e != 0) o *= exp2(float(e));
  return o;
}
)";

std::string Translator::Run() {
  Scan();
  info_.pixel = pixel_;
  uint32_t nregs = dynamic_regs_ ? 64 : std::max<uint32_t>(max_reg_ + 1, 16);
  if (pixel_) nregs = std::max<uint32_t>(nregs, 16);
  nregs = std::min<uint32_t>(nregs, 64);
  if (dynamic_regs_) nregs = 64;
  info_.reg_count = nregs;

  // Body first (collects texture bindings), header after.
  bool sm = needs_state_machine_;
  if (sm) {
    Line("uint pc = 0u;");
    Line("[loop] while (pc != 0xFFFFu) {");
    ++indent_;
    Line("[branch] switch (pc) {");  // [forcecase] trips an FXC internal error
    std::vector<uint32_t> labels(labels_.begin(), labels_.end());
    for (size_t li = 0; li < labels.size(); ++li) {
      uint32_t start = labels[li];
      if (start >= cf_count_) continue;
      uint32_t end = li + 1 < labels.size() ? std::min(labels[li + 1], cf_count_) : cf_count_;
      Line(Fmt("case %uu: {", start));
      ++indent_;
      for (uint32_t i = start; i < end; ++i) EmitCf(i, true);
      Line(Fmt("pc = %uu; break;", end < cf_count_ ? end : 0xFFFFu));
      --indent_;
      Line("}");
    }
    Line("default: pc = 0xFFFFu; break;");
    Line("}");
    --indent_;
    Line("}");
  } else {
    Line("do {");  // [loop] here crashes FXC on some shaders (dynamic constant indexing)
    ++indent_;
    for (uint32_t i = 0; i < cf_count_; ++i) EmitCf(i, false);
    --indent_;
    Line("} while (false);");
  }
  std::string body = out_.str();

  std::ostringstream h;
  h << kCommonHlsl;
  if (!pixel_) h << "ByteAddressBuffer xvmem : register(t0);\n";
  for (uint32_t i = 0; i < info_.textures.size(); ++i) {
    const TextureBinding& t = info_.textures[i];
    const char* type = t.dim == TexDim::k3D     ? "Texture3D"
                       : t.dim == TexDim::kCube ? "TextureCube"
                                                : "Texture2DArray";
    h << type << " xt" << i << " : register(t" << (i + 1) << ");\n";
    h << "SamplerState xs" << i << " : register(s" << i << ");\n";
    if (t.dim == TexDim::k3D) {
      h << "Texture2DArray xt" << i << "a : register(t" << (i + 1 + kStackedSrvBase) << ");\n";
    }
  }

  if (!pixel_) {
    h << R"(
XVSOut main(uint vid : SV_VertexID) {
  XVSOut xo;
  xo.pos = float4(0.0, 0.0, 0.0, 1.0);
  [unroll] for (int oi = 0; oi < 16; ++oi) xo.o[oi] = float4(0.0, 0.0, 0.0, 0.0);
  xo.pts = float4(0.0, 0.0, 0.0, 0.0);
  float4 xopos = float4(0.0, 0.0, 0.0, 1.0);
)";
  } else {
    h << R"(
struct XPSOut {
  float4 c0 : SV_Target0;
  float4 c1 : SV_Target1;
  float4 c2 : SV_Target2;
  float4 c3 : SV_Target3;
)";
    if (info_.writes_depth) h << "  float depth : SV_Depth;\n";
    h << R"(};
XPSOut main(XVSOut xi, bool xface : SV_IsFrontFace) {
  float4 oc0 = float4(0.0, 0.0, 0.0, 0.0), oc1 = oc0, oc2 = oc0, oc3 = oc0;
  float odepth = xi.pos.z;
)";
  }
  h << "  float4 r[" << nregs << "];\n";
  h << "  [unroll] for (int ri = 0; ri < " << nregs << "; ++ri) r[ri] = float4(0.0, 0.0, 0.0, 0.0);\n";
  if (!pixel_) {
    h << "  r[0].x = float(vid);\n";
  } else {
    h << "  [unroll] for (int ii = 0; ii < " << std::min<uint32_t>(nregs, 16) << "; ++ii) r[ii] = xi.o[ii];\n";
    if (opt_.ps_param_gen_reg < 16 && opt_.ps_param_gen_reg < nregs) {
      // X: pixel x (sign = back face), Y: pixel y (sign = point), Z: sign = line, W: 0.
      // In guest pixels: render targets may be drawn at a higher internal resolution (xsys[6].x).
      h << "  {\n    float4 pg = float4(floor(xi.pos.xy * xsys[6].x), 0.0, 0.0);\n";
      h << "    uint fl = asuint(xsys[3].z);\n";
      h << "    if ((fl & 1u) && !xface) pg.x = -pg.x;\n";
      h << "    if (fl & 2u) { pg.y = -pg.y; pg.zw = xi.pts.zw; }\n";
      h << "    if (fl & 4u) pg.z = -0.0 - pg.z;\n";
      h << "    r[" << opt_.ps_param_gen_reg << "] = pg;\n  }\n";
    }
  }
  h << "  float ps = 0.0;\n  bool p0 = false;\n  int a0 = 0;\n  int aL = 0;\n  int lc = 0;\n";
  h << "  int lst[4] = {0, 0, 0, 0};\n  int lit[4] = {0, 0, 0, 0};\n  int lsp = 0;\n";
  h << "  uint cst[4] = {0u, 0u, 0u, 0u};\n  int csp = 0;\n";
  h << "  uint vfb = 0u, vfe = 0u;\n  float xlod = 0.0;\n  float3 xgh = float3(0.0, 0.0, 0.0), xgv = xgh;\n";
  h << body;

  if (!pixel_) {
    h << R"(
  // Xenos viewport transform (PA_CL_VTE_CNTL) -> D3D11 clip space of the host viewport, whose
  // origin is xsys[5].xy and whose size is 2 / xsys[1].zw.
  uint vte = asuint(xsys[2].x);
  float w = (vte & 4u) ? xopos.w : 1.0 / xopos.w;
  float3 nd = xopos.xyz;
  if (!(vte & 1u)) nd.xy /= w;
  if (!(vte & 2u)) nd.z /= w;
  float2 scr = nd.xy * float2(xsys[0].x, xsys[0].z) + float2(xsys[0].y, xsys[0].w);
  scr += xsys[2].y + xsys[2].zw;
  float zz = nd.z * xsys[1].x + xsys[1].y;
  float2 rel = (scr - xsys[5].xy) * xsys[1].zw;
  float2 cl = float2(rel.x - 1.0, 1.0 - rel.y);
  xo.pos = float4(cl * w, zz * w, w);
  if (xo.pts.z != 0.0) xo.pos = asfloat(0x7FC00000u).xxxx;
  return xo;
}
)";
  } else {
    h << R"(
  // Alpha test (RB_COLORCONTROL): func 0 never .. 7 always.
  uint af = asuint(xsys[3].x);
  if (af != 7u) {
    float a = oc0.a, ar = xsys[3].y;
    bool xpass = (af == 1u && a < ar) || (af == 2u && a == ar) || (af == 3u && a <= ar) ||
                (af == 4u && a > ar) || (af == 5u && a != ar) || (af == 6u && a >= ar);
    if (!xpass) discard;
  }
  XPSOut xr;
  xr.c0 = oc0 * xsys[4].x;
  xr.c1 = oc1 * xsys[4].y;
  xr.c2 = oc2 * xsys[4].z;
  xr.c3 = oc3 * xsys[4].w;
)";
    if (info_.writes_depth) h << "  xr.depth = saturate(odepth);\n";
    h << "  return xr;\n}\n";
  }
  info_.ok = err_.empty();
  info_.error = err_;
  return h.str();
}

}  // namespace

std::string TranslateXenosShader(const uint32_t* ucode, uint32_t dword_count, bool pixel,
                                 const TranslateOptions& options, ShaderInfo& info) {
  info = ShaderInfo();
  Translator t(ucode, dword_count, pixel, options, info);
  return t.Run();
}

}  // namespace bf2n
