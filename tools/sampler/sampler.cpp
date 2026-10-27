// Minimal sampling profiler: sampler.exe <pid> <seconds> <out.txt> [hz]
// Suspends every thread of the target at ~hz, walks its stack (StackWalk64 + PDB symbols) and
// reports per-thread busy share plus self/inclusive hot functions.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

struct ThreadInfo {
  HANDLE h = nullptr;
  uint64_t samples = 0, busy = 0;
  FILETIME kernel{}, user{};
  std::unordered_map<uint64_t, uint64_t> self, incl;
  std::map<std::pair<uint64_t,uint64_t>, uint64_t> edges;
  std::unordered_map<uint64_t, uint64_t> leaf_pc, first_host;
  std::wstring name;
  uint64_t start_cpu = 0, last_cpu = 0;
  bool active = true;
};

static HANDLE g_proc;
static std::unordered_map<uint64_t, std::string> g_line_cache;
static std::string LineOf(uint64_t pc) {
  auto it = g_line_cache.find(pc);
  if (it != g_line_cache.end()) return it->second;
  IMAGEHLP_LINE64 l{sizeof(l)};
  DWORD disp = 0;
  std::string r;
  if (SymGetLineFromAddr64(g_proc, pc, &disp, &l)) {
    std::string f = l.FileName;
    size_t k = f.find_last_of("\\/");
    r = f.substr(k == std::string::npos ? 0 : k + 1) + ":" + std::to_string(l.LineNumber);
  }
  return g_line_cache[pc] = r;
}
static bool IsOurs(uint64_t pc) {
  std::string s = LineOf(pc);
  return s.rfind("renderer.cpp", 0) == 0 || s.rfind("textures.cpp", 0) == 0 || s.rfind("gpu_hooks.cpp", 0) == 0 ||
         s.rfind("pc_settings.cpp", 0) == 0 || s.rfind("parallel_jobs.h", 0) == 0 || s.rfind("texture_copy.h", 0) == 0;
}
static uint64_t Ft(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

int main(int argc, char** argv) {
  if (argc < 4) { printf("usage: sampler pid seconds out [hz]\n"); return 1; }
  DWORD pid = strtoul(argv[1], nullptr, 10);
  double seconds = atof(argv[2]);
  int hz = argc > 4 ? atoi(argv[4]) : 1000;
  DWORD only_tid = argc > 5 ? strtoul(argv[5], nullptr, 10) : 0;
  HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!proc) { printf("OpenProcess failed %lu\n", GetLastError()); return 1; }
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(proc, nullptr, TRUE);
  g_proc = proc;
  timeBeginPeriod(1);

  std::map<DWORD, ThreadInfo> threads;
  auto refresh = [&] {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid || threads.count(te.th32ThreadID)) continue;
      if (only_tid && te.th32ThreadID != only_tid) continue;
      HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
      if (!h) continue;
      ThreadInfo& t = threads[te.th32ThreadID];
      t.h = h;
      PWSTR desc = nullptr;
      if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) { t.name = desc; LocalFree(desc); }
      FILETIME c, e, k, u;
      GetThreadTimes(h, &c, &e, &k, &u);
      t.start_cpu = Ft(k) + Ft(u);
    }
    CloseHandle(snap);
  };
  refresh();
  LARGE_INTEGER f, t0, now;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  uint64_t ticks = 0;
  std::unordered_map<uint64_t, uint64_t> fn_cache;
  for (;;) {
    QueryPerformanceCounter(&now);
    double el = double(now.QuadPart - t0.QuadPart) / f.QuadPart;
    if (el >= seconds) break;
    if ((ticks++ % 250) == 0) {
      refresh();
      // Only walk threads that burned >1% CPU since the last check; idle ones are just waits.
      static LARGE_INTEGER last{};
      double dt = last.QuadPart ? double(now.QuadPart - last.QuadPart) / f.QuadPart : 0;
      for (auto& [tid, t] : threads) {
        FILETIME c, e, k, u;
        GetThreadTimes(t.h, &c, &e, &k, &u);
        uint64_t cpu = Ft(k) + Ft(u);
        t.active = dt == 0 || double(cpu - t.last_cpu) / 1e7 > dt * 0.01;
        t.last_cpu = cpu;
      }
      last = now;
    }
    for (auto& [tid, t] : threads) {
      if (!t.active) continue;
      if (SuspendThread(t.h) == DWORD(-1)) continue;
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t.h, &ctx)) {
        t.samples++;
        STACKFRAME64 sf{};
        sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        std::vector<uint64_t> pcs;
        for (int i = 0; i < 48; i++) {
          if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, t.h, &sf, &ctx, nullptr,
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
          if (!sf.AddrPC.Offset) break;
          pcs.push_back(sf.AddrPC.Offset);
        }
        ResumeThread(t.h);
        // Resolve to function start addresses so samples aggregate per function.
        std::vector<uint64_t> fns;
        for (uint64_t pc : pcs) {
          char buf[sizeof(SYMBOL_INFO) + 256];
          auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
          si->SizeOfStruct = sizeof(SYMBOL_INFO);
          si->MaxNameLen = 255;
          DWORD64 disp = 0;
          auto it = fn_cache.find(pc);
          if (it == fn_cache.end())
            it = fn_cache.emplace(pc, SymFromAddr(proc, pc, &disp, si) ? si->Address : (pc & ~0xFFFull) | 1).first;
          fns.push_back(it->second);
        }
        if (!fns.empty()) {
          t.self[fns[0]]++;
          t.leaf_pc[pcs[0]]++;
          for (uint64_t pc : pcs) if (IsOurs(pc)) { t.first_host[pc]++; break; }
          for (size_t i = 0; i + 1 < fns.size() && i < 6; i++) if (fns[i] != fns[i+1]) t.edges[{fns[i+1], fns[i]}]++;
          std::set<uint64_t> seen(fns.begin(), fns.end());
          for (uint64_t a : seen) t.incl[a]++;
        }
      } else {
        ResumeThread(t.h);
      }
    }
    if (hz < 1000) Sleep(1000 / hz); else { LARGE_INTEGER a, b; QueryPerformanceCounter(&a); do QueryPerformanceCounter(&b); while (double(b.QuadPart - a.QuadPart) / f.QuadPart < 1.0 / hz); }
  }
  FILE* out = fopen(argv[3], "w");
  auto name = [&](uint64_t a) -> std::string {
    if (a & 1) { char b[64]; sprintf(b, "?%llx", (unsigned long long)(a & ~1ull)); return b; }
    char buf[sizeof(SYMBOL_INFO) + 256];
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(proc, a, &disp, si)) return "?";
    IMAGEHLP_MODULE64 m{sizeof(m)};
    std::string mod = SymGetModuleInfo64(proc, a, &m) ? m.ModuleName : "";
    return mod + "!" + si->Name;
  };
  std::vector<std::pair<uint64_t, DWORD>> order;
  for (auto& [tid, t] : threads) {
    FILETIME c, e, k, u;
    GetThreadTimes(t.h, &c, &e, &k, &u);
    t.busy = (Ft(k) + Ft(u) - t.start_cpu) / 10000;  // ms of CPU during the window
    order.push_back({t.busy, tid});
  }
  std::sort(order.rbegin(), order.rend());
  fprintf(out, "window %.1fs, hz %d\n", seconds, hz);
  for (auto& [busy, tid] : order) {
    ThreadInfo& t = threads[tid];
    if (busy < seconds * 1000 * 0.02) continue;
    fprintf(out, "\n=== thread %lu '%ls' cpu %.1f%% samples %llu\n", tid, t.name.c_str(),
            100.0 * busy / (seconds * 1000), (unsigned long long)t.samples);
    std::vector<std::pair<uint64_t, uint64_t>> s(t.self.begin(), t.self.end()), in(t.incl.begin(), t.incl.end());
    auto by = [](auto& a, auto& b) { return a.second > b.second; };
    std::sort(s.begin(), s.end(), by);
    std::sort(in.begin(), in.end(), by);
    fprintf(out, "-- self\n");
    for (size_t i = 0; i < s.size() && i < 40; i++)
      fprintf(out, "%6.2f%% %s\n", 100.0 * s[i].second / t.samples, name(s[i].first).c_str());
    fprintf(out, "-- inclusive\n");
    for (size_t i = 0; i < in.size() && i < 60; i++)
      fprintf(out, "%6.2f%% %s\n", 100.0 * in[i].second / t.samples, name(in[i].first).c_str());
    auto by_line = [&](std::unordered_map<uint64_t, uint64_t>& m, const char* title) {
      std::map<std::string, uint64_t> agg;
      for (auto& [pc, n] : m) { std::string l = LineOf(pc); if (l.empty()) { IMAGEHLP_MODULE64 mi{sizeof(mi)}; char b[300];
          if (SymGetModuleInfo64(proc, pc, &mi)) sprintf(b, "%s+0x%llx", mi.ModuleName, (unsigned long long)(pc - mi.BaseOfImage));
          else sprintf(b, "%llx", (unsigned long long)pc);
          l = b; }
        agg[l] += n; }
      std::vector<std::pair<std::string, uint64_t>> v(agg.begin(), agg.end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      fprintf(out, "-- %s\n", title);
      for (size_t i = 0; i < v.size(); i++) fprintf(out, "%6.2f%% %s\n", 100.0 * v[i].second / t.samples, v[i].first.c_str());
    };
    by_line(t.leaf_pc, "leaf by line");
    by_line(t.first_host, "innermost renderer source line on stack");
    fprintf(out, "-- edges caller -> callee (near leaf)\n");
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> ed(t.edges.begin(), t.edges.end());
    std::sort(ed.begin(), ed.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < ed.size() && i < 100; i++)
      fprintf(out, "%6.2f%% %s -> %s\n", 100.0 * ed[i].second / t.samples, name(ed[i].first.first).c_str(),
              name(ed[i].first.second).c_str());
  }
  fclose(out);
  return 0;
}
