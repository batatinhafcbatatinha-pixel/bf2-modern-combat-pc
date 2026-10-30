// Linux implementation of the Win32 subset in native_win32_posix.h (PC settings screen).
// Text: FreeType with the font fontconfig picks for the requested face (Segoe UI -> its metric-compatible or
// default sans substitute), cached per size/weight/slant.
#ifndef _WIN32
#include "native_win32_posix.h"

#include <SDL3/SDL.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {

enum class Kind { kDC, kBitmap, kFont, kBrush, kPen };

struct Object {
  explicit Object(Kind k) : kind(k) {}
  virtual ~Object() = default;
  Kind kind;
};

struct Bitmap final : Object {
  Bitmap() : Object(Kind::kBitmap) {}
  int w = 0, h = 0;
  std::vector<uint32_t> pixels;
};

struct Font final : Object {
  Font() : Object(Kind::kFont) {}
  FT_Face face = nullptr;
  int px = 12;
  int ascent = 0, descent = 0;
  bool synthetic_bold = false;
  ~Font() override {
    if (face) FT_Done_Face(face);
  }
};

struct Brush final : Object {
  Brush(Kind k, COLORREF c) : Object(k), color(c) {}
  COLORREF color;
};

struct DC final : Object {
  DC() : Object(Kind::kDC) {}
  Bitmap* bitmap = nullptr;
  Font* font = nullptr;
  Brush* brush = nullptr;
  Brush* pen = nullptr;
  COLORREF text = 0;
};

std::mutex g_mutex;
FT_Library g_ft = nullptr;
thread_local DWORD g_last_error = 0;

FT_Library Library() {
  if (!g_ft) FT_Init_FreeType(&g_ft);
  return g_ft;
}

uint32_t Bgr(COLORREF c) {  // COLORREF 0x00BBGGRR -> DIB pixel 0x00RRGGBB
  return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
}

std::string Narrow(const wchar_t* s, int n = -1) {
  std::string out;
  if (!s) return out;
  for (int i = 0; n < 0 ? s[i] != 0 : i < n; ++i) {
    char32_t c = char32_t(s[i]);
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    char32_t v;
    int extra;
    if (c < 0x80) { v = c; extra = 0; }
    else if ((c >> 5) == 6) { v = c & 0x1F; extra = 1; }
    else if ((c >> 4) == 14) { v = c & 0x0F; extra = 2; }
    else { v = c & 0x07; extra = 3; }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) v = (v << 6) | ((unsigned char)s[i] & 0x3F);
    out += wchar_t(v);
  }
  return out;
}

// fontconfig: the file that best matches the face name, weight and slant.
std::string MatchFont(const std::string& family, int weight, bool italic, int* face_index) {
  static bool init = FcInit();
  (void)init;
  FcPattern* pattern = FcPatternCreate();
  FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
  FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif"));
  FcPatternAddInteger(pattern, FC_WEIGHT, weight >= 800 ? FC_WEIGHT_HEAVY : weight >= 600 ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
  FcPatternAddInteger(pattern, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcPattern* match = FcFontMatch(nullptr, pattern, &result);
  std::string file;
  if (match) {
    FcChar8* path = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch && path) file = reinterpret_cast<char*>(path);
    int index = 0;
    FcPatternGetInteger(match, FC_INDEX, 0, &index);
    if (face_index) *face_index = index;
    FcPatternDestroy(match);
  }
  FcPatternDestroy(pattern);
  return file;
}

template <class T> T* As(HGDIOBJ h, Kind k) {
  auto* o = static_cast<Object*>(h);
  return o && o->kind == k ? static_cast<T*>(o) : nullptr;
}

// Glyph rendering. Returns the advance in pixels.
int GlyphAdvance(Font& f, char32_t c) {
  FT_UInt index = FT_Get_Char_Index(f.face, c);
  if (!index && c != ' ') index = FT_Get_Char_Index(f.face, '?');
  if (FT_Load_Glyph(f.face, index, FT_LOAD_DEFAULT)) return 0;
  int adv = int((f.face->glyph->advance.x + 32) >> 6);
  return f.synthetic_bold ? adv + 1 : adv;
}

int TextWidth(Font& f, const wchar_t* s, int n) {
  int w = 0;
  for (int i = 0; i < n; ++i) w += GlyphAdvance(f, char32_t(s[i]));
  return w;
}

void Blend(Bitmap& b, int x, int y, uint32_t rgb, int alpha, const RECT* clip) {
  if (x < 0 || y < 0 || x >= b.w || y >= b.h || alpha <= 0) return;
  if (clip && (x < clip->left || x >= clip->right || y < clip->top || y >= clip->bottom)) return;
  uint32_t& d = b.pixels[size_t(y) * size_t(b.w) + size_t(x)];
  if (alpha >= 255) { d = (d & 0xFF000000u) | rgb; return; }
  uint32_t out = d & 0xFF000000u;
  for (int sh = 0; sh <= 16; sh += 8) {
    int s = int((rgb >> sh) & 0xFF), t = int((d >> sh) & 0xFF);
    out |= uint32_t((t * (255 - alpha) + s * alpha + 127) / 255) << sh;
  }
  d = out;
}

void DrawGlyphs(DC& dc, const wchar_t* s, int n, int x, int baseline, const RECT* clip) {
  Font& f = *dc.font;
  Bitmap& b = *dc.bitmap;
  const uint32_t rgb = Bgr(dc.text);
  for (int i = 0; i < n; ++i) {
    FT_UInt index = FT_Get_Char_Index(f.face, char32_t(s[i]));
    if (!index && s[i] != L' ') index = FT_Get_Char_Index(f.face, '?');
    if (FT_Load_Glyph(f.face, index, FT_LOAD_DEFAULT) || FT_Render_Glyph(f.face->glyph, FT_RENDER_MODE_NORMAL)) continue;
    FT_GlyphSlot g = f.face->glyph;
    const FT_Bitmap& bm = g->bitmap;
    for (int pass = 0; pass < (f.synthetic_bold ? 2 : 1); ++pass)
      for (unsigned row = 0; row < bm.rows; ++row)
        for (unsigned col = 0; col < bm.width; ++col)
          Blend(b, x + g->bitmap_left + int(col) + pass, baseline - g->bitmap_top + int(row), rgb,
                bm.buffer[row * unsigned(std::abs(bm.pitch)) + col], clip);
    x += int((g->advance.x + 32) >> 6) + (f.synthetic_bold ? 1 : 0);
  }
}

void FillSpan(Bitmap& b, int y, int x0, int x1, uint32_t rgb) {
  if (y < 0 || y >= b.h) return;
  x0 = std::max(x0, 0);
  x1 = std::min(x1, b.w);
  for (int x = x0; x < x1; ++x) {
    uint32_t& d = b.pixels[size_t(y) * size_t(b.w) + size_t(x)];
    d = (d & 0xFF000000u) | rgb;
  }
}

// Even-odd scanline fill of a polygon (pixel centres), then its one-pixel outline.
void FillPolygon(Bitmap& b, const std::vector<std::pair<double, double>>& p, uint32_t fill) {
  if (p.size() < 3) return;
  double ymin = p[0].second, ymax = p[0].second;
  for (auto& q : p) { ymin = std::min(ymin, q.second); ymax = std::max(ymax, q.second); }
  for (int y = std::max(0, int(std::floor(ymin))); y <= std::min(b.h - 1, int(std::ceil(ymax))); ++y) {
    const double cy = y + 0.5;
    std::vector<double> xs;
    for (size_t i = 0; i < p.size(); ++i) {
      auto a = p[i], c = p[(i + 1) % p.size()];
      if ((a.second <= cy && c.second > cy) || (c.second <= cy && a.second > cy))
        xs.push_back(a.first + (cy - a.second) * (c.first - a.first) / (c.second - a.second));
    }
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2)
      FillSpan(b, y, int(std::lround(xs[i])), int(std::lround(xs[i + 1])), fill);
  }
}

void Line(Bitmap& b, int x0, int y0, int x1, int y1, uint32_t rgb) {
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    if (x0 >= 0 && y0 >= 0 && x0 < b.w && y0 < b.h) {
      uint32_t& d = b.pixels[size_t(y0) * size_t(b.w) + size_t(x0)];
      d = (d & 0xFF000000u) | rgb;
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

std::string IniPath(const wchar_t* file) {
  std::string p = Narrow(file);
  std::replace(p.begin(), p.end(), '\\', '/');
  return p;
}

using IniData = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;

IniData ReadIni(const std::string& path) {
  IniData data;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
      data.push_back({line.substr(1, line.size() - 2), {}});
    } else if (auto eq = line.find('='); eq != std::string::npos && !data.empty()) {
      data.back().second.push_back({line.substr(0, eq), line.substr(eq + 1)});
    }
  }
  return data;
}

bool SameKey(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                             [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

}  // namespace

HDC CreateCompatibleDC(HDC) { return new DC(); }
BOOL DeleteDC(HDC dc) {
  delete As<DC>(dc, Kind::kDC);
  return TRUE;
}

HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, UINT, void** bits, HANDLE, DWORD) {
  if (!bi || bi->bmiHeader.biBitCount != 32 || bi->bmiHeader.biWidth <= 0 || bi->bmiHeader.biHeight == 0) return nullptr;
  auto* b = new Bitmap();
  b->w = bi->bmiHeader.biWidth;
  b->h = std::abs(bi->bmiHeader.biHeight);  // callers use top-down DIBs (negative height)
  b->pixels.assign(size_t(b->w) * size_t(b->h), 0);
  if (bits) *bits = b->pixels.data();
  return b;
}

HGDIOBJ SelectObject(HDC h, HGDIOBJ obj) {
  DC* dc = As<DC>(h, Kind::kDC);
  auto* o = static_cast<Object*>(obj);
  if (!dc || !o) return nullptr;
  HGDIOBJ old = nullptr;
  switch (o->kind) {
    case Kind::kBitmap: old = dc->bitmap; dc->bitmap = static_cast<Bitmap*>(o); break;
    case Kind::kFont: old = dc->font; dc->font = static_cast<Font*>(o); break;
    case Kind::kBrush: old = dc->brush; dc->brush = static_cast<Brush*>(o); break;
    case Kind::kPen: old = dc->pen; dc->pen = static_cast<Brush*>(o); break;
    default: break;
  }
  return old;
}

BOOL DeleteObject(HGDIOBJ obj) {
  auto* o = static_cast<Object*>(obj);
  if (!o || o->kind == Kind::kDC) return FALSE;
  delete o;
  return TRUE;
}

HFONT CreateFontW(int height, int, int, int, int weight, DWORD italic, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                  const wchar_t* face) {
  std::lock_guard<std::mutex> lock(g_mutex);
  int index = 0;
  std::string file = MatchFont(face ? Narrow(face) : "sans-serif", weight, italic != 0, &index);
  auto* f = new Font();
  if (file.empty() || FT_New_Face(Library(), file.c_str(), index, &f->face) || !f->face) {
    delete f;
    return nullptr;
  }
  FT_Select_Charmap(f->face, FT_ENCODING_UNICODE);
  // Negative height: character height (em) in pixels, as the callers pass it.
  f->px = std::max(1, std::abs(height));
  FT_Set_Pixel_Sizes(f->face, 0, FT_UInt(f->px));
  f->ascent = int((f->face->size->metrics.ascender + 63) >> 6);
  f->descent = int((-f->face->size->metrics.descender + 63) >> 6);
  f->synthetic_bold = weight >= 600 && !(f->face->style_flags & FT_STYLE_FLAG_BOLD);
  return f;
}

COLORREF SetTextColor(HDC h, COLORREF c) {
  DC* dc = As<DC>(h, Kind::kDC);
  if (!dc) return 0;
  COLORREF old = dc->text;
  dc->text = c;
  return old;
}

int SetBkMode(HDC, int) { return TRANSPARENT; }

BOOL GetTextExtentPoint32W(HDC h, const wchar_t* s, int n, SIZE* size) {
  DC* dc = As<DC>(h, Kind::kDC);
  if (!dc || !dc->font || !size) return FALSE;
  std::lock_guard<std::mutex> lock(g_mutex);
  size->cx = TextWidth(*dc->font, s, n);
  size->cy = dc->font->ascent + dc->font->descent;
  return TRUE;
}

int DrawTextW(HDC h, const wchar_t* s, int n, RECT* r, UINT format) {
  DC* dc = As<DC>(h, Kind::kDC);
  if (!dc || !dc->font || !dc->bitmap || !s || !r) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  Font& f = *dc->font;
  std::wstring text(s, n < 0 ? std::wcslen(s) : size_t(n));
  const int box = r->right - r->left;
  int width = TextWidth(f, text.data(), int(text.size()));
  if ((format & DT_END_ELLIPSIS) && width > box) {
    const std::wstring dots = L"...";
    const int dots_w = TextWidth(f, dots.data(), 3);
    while (!text.empty() && TextWidth(f, text.data(), int(text.size())) + dots_w > box) text.pop_back();
    text += dots;
    width = TextWidth(f, text.data(), int(text.size()));
  }
  int x = r->left;
  if (format & DT_CENTER) x = r->left + (box - width) / 2;
  else if (format & DT_RIGHT) x = r->right - width;
  const int line = f.ascent + f.descent;
  int top = r->top;
  if (format & DT_VCENTER) top = r->top + (r->bottom - r->top - line) / 2;
  DrawGlyphs(*dc, text.data(), int(text.size()), x, top + f.ascent, nullptr);
  return line;
}

HBRUSH CreateSolidBrush(COLORREF c) { return new Brush(Kind::kBrush, c); }
HPEN CreatePen(int, int, COLORREF c) { return new Brush(Kind::kPen, c); }

int FillRect(HDC h, const RECT* r, HBRUSH brush) {
  DC* dc = As<DC>(h, Kind::kDC);
  Brush* b = As<Brush>(brush, Kind::kBrush);
  if (!dc || !dc->bitmap || !r || !b) return 0;
  for (int y = r->top; y < r->bottom; ++y) FillSpan(*dc->bitmap, y, r->left, r->right, Bgr(b->color));
  return 1;
}

BOOL Polygon(HDC h, const POINT* pts, int count) {
  DC* dc = As<DC>(h, Kind::kDC);
  if (!dc || !dc->bitmap || !pts || count < 3) return FALSE;
  std::vector<std::pair<double, double>> p;
  for (int i = 0; i < count; ++i) p.push_back({double(pts[i].x), double(pts[i].y)});
  if (dc->brush) FillPolygon(*dc->bitmap, p, Bgr(dc->brush->color));
  if (dc->pen)
    for (int i = 0; i < count; ++i)
      Line(*dc->bitmap, pts[i].x, pts[i].y, pts[(i + 1) % count].x, pts[(i + 1) % count].y, Bgr(dc->pen->color));
  return TRUE;
}

BOOL Ellipse(HDC h, int left, int top, int right, int bottom) {
  DC* dc = As<DC>(h, Kind::kDC);
  if (!dc || !dc->bitmap || right <= left || bottom <= top) return FALSE;
  const double cx = (left + right) / 2.0, cy = (top + bottom) / 2.0, rx = (right - left) / 2.0, ry = (bottom - top) / 2.0;
  std::vector<std::pair<double, double>> p;
  const int n = 64;
  for (int i = 0; i < n; ++i) {
    double a = 2.0 * M_PI * i / n;
    p.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
  }
  if (dc->brush) FillPolygon(*dc->bitmap, p, Bgr(dc->brush->color));
  if (dc->pen)
    for (int i = 0; i < n; ++i)
      Line(*dc->bitmap, int(std::lround(p[i].first)), int(std::lround(p[i].second)), int(std::lround(p[(i + 1) % n].first)),
           int(std::lround(p[(i + 1) % n].second)), Bgr(dc->pen->color));
  return TRUE;
}

int FoldStringW(DWORD, const wchar_t* src, int n, wchar_t* dst, int dst_n) {
  // Only MAP_COMPOSITE on one Latin-1/Latin Extended-A letter is used: split it into base letter + accent.
  if (!src || !dst || n < 1 || dst_n < 1) return 0;
  static const wchar_t* const kFrom = L"ÀÁÂÃÄÅÇÈÉÊËÌÍÎÏÑÒÓÔÕÖÙÚÛÜÝàáâãäåçèéêëìíîïñòóôõöùúûüýÿ";
  static const wchar_t* const kBase = L"AAAAAACEEEEIIIINOOOOOUUUUYaaaaaaceeeeiiiinooooouuuuyy";
  const wchar_t* at = std::wcschr(kFrom, src[0]);
  if (!at || dst_n < 2) { dst[0] = src[0]; return 1; }
  dst[0] = kBase[at - kFrom];
  dst[1] = 0x0301;  // a combining mark; callers only use the base letter
  return 2;
}

BOOL GetCursorPos(POINT* p) {
  if (!p) return FALSE;
  float x = 0, y = 0;
  SDL_GetGlobalMouseState(&x, &y);
  p->x = LONG(x);
  p->y = LONG(y);
  return TRUE;
}

BOOL ScreenToClient(HWND w, POINT* p) {
  if (!w || !p) return FALSE;
  int wx = 0, wy = 0;
  if (!SDL_GetWindowPosition(static_cast<SDL_Window*>(w), &wx, &wy)) return FALSE;
  // The settings screen measures in pixels; SDL positions are in window coordinates.
  float density = SDL_GetWindowPixelDensity(static_cast<SDL_Window*>(w));
  if (density <= 0) density = 1;
  p->x = LONG((p->x - wx) * density);
  p->y = LONG((p->y - wy) * density);
  return TRUE;
}

HWND GetForegroundWindow() { return SDL_GetKeyboardFocus(); }
BOOL IsWindowVisible(HWND w) {
  return w && !(SDL_GetWindowFlags(static_cast<SDL_Window*>(w)) & SDL_WINDOW_HIDDEN);
}
BOOL IsIconic(HWND w) {
  return w && (SDL_GetWindowFlags(static_cast<SDL_Window*>(w)) & SDL_WINDOW_MINIMIZED);
}

DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size) {
  const char* v = name ? std::getenv(name) : nullptr;
  if (!v) { g_last_error = 203; return 0; }  // ERROR_ENVVAR_NOT_FOUND
  DWORD n = DWORD(std::strlen(v));
  if (!buffer || n + 1 > size) return n + 1;
  std::memcpy(buffer, v, n + 1);
  return n;
}

BOOL SetEnvironmentVariableA(const char* name, const char* value) {
  if (!name) return FALSE;
  return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

UINT GetPrivateProfileIntW(const wchar_t* section, const wchar_t* key, int def, const wchar_t* file) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string s = Narrow(section), k = Narrow(key);
  for (auto& sec : ReadIni(IniPath(file))) {
    if (!SameKey(sec.first, s)) continue;
    for (auto& kv : sec.second)
      if (SameKey(kv.first, k)) {
        char* end = nullptr;
        long v = std::strtol(kv.second.c_str(), &end, 10);
        return end == kv.second.c_str() ? UINT(def) : UINT(v);
      }
  }
  return UINT(def);
}

BOOL WritePrivateProfileStringW(const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* file) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string path = IniPath(file), s = Narrow(section), k = Narrow(key);
  IniData data = ReadIni(path);
  auto sec = std::find_if(data.begin(), data.end(), [&](auto& e) { return SameKey(e.first, s); });
  if (sec == data.end()) { data.push_back({s, {}}); sec = data.end() - 1; }
  auto kv = std::find_if(sec->second.begin(), sec->second.end(), [&](auto& e) { return SameKey(e.first, k); });
  if (!value) {
    if (kv != sec->second.end()) sec->second.erase(kv);
  } else if (kv != sec->second.end()) {
    kv->second = Narrow(value);
  } else {
    sec->second.push_back({k, Narrow(value)});
  }
  std::ostringstream out;
  for (auto& e : data) {
    out << '[' << e.first << "]\n";
    for (auto& p : e.second) out << p.first << '=' << p.second << '\n';
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << out.str();
    if (!f) return FALSE;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

DWORD GetModuleFileNameA(HMODULE, char* out, DWORD n) {
  if (!out || !n) return 0;
  ssize_t len = readlink("/proc/self/exe", out, n - 1);
  if (len < 0) { g_last_error = DWORD(errno); out[0] = 0; return 0; }
  out[len] = 0;
  return DWORD(len);
}

DWORD GetModuleFileNameW(HMODULE m, wchar_t* out, DWORD n) {
  char path[PATH_MAX];
  if (!out || !n || !GetModuleFileNameA(m, path, sizeof(path))) return 0;
  std::wstring w = Widen(path);
  size_t len = std::min<size_t>(w.size(), n - 1);
  std::wmemcpy(out, w.data(), len);
  out[len] = 0;
  return DWORD(len);
}

const wchar_t* GetCommandLineW() {
  static const std::wstring line = [] {
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::replace(raw.begin(), raw.end(), '\0', '\x1f');  // argument separator, see CreateProcessW
    return Widen(raw);
  }();
  return line.c_str();
}

BOOL CreateProcessW(const wchar_t* app, const wchar_t* cmdline, void*, void*, BOOL, DWORD, void*, const wchar_t*,
                    STARTUPINFOW*, PROCESS_INFORMATION* pi) {
  // Used to restart the game: replace this process image with a new one (same arguments).
  std::vector<std::string> args;
  std::string line = cmdline ? Narrow(cmdline) : std::string();
  std::string cur;
  for (char c : line) {
    if (c == '\x1f') { args.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) args.push_back(cur);
  std::string exe = app ? Narrow(app) : (args.empty() ? std::string() : args[0]);
  if (exe.empty()) { g_last_error = 2; return FALSE; }
  if (args.empty()) args.push_back(exe);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  execv(exe.c_str(), argv.data());
  g_last_error = DWORD(errno);
  if (pi) *pi = {};
  return FALSE;
}

void ExitProcess(UINT code) { std::_Exit(int(code)); }

WORD GetUserDefaultUILanguage() {
  const char* v = std::getenv("LC_ALL");
  if (!v || !*v) v = std::getenv("LC_MESSAGES");
  if (!v || !*v) v = std::getenv("LANG");
  std::string l = v ? v : "";
  auto is = [&](const char* p) { return l.rfind(p, 0) == 0; };
  if (is("pt")) return LANG_PORTUGUESE;
  if (is("es")) return LANG_SPANISH;
  if (is("fr")) return LANG_FRENCH;
  if (is("de")) return LANG_GERMAN;
  if (is("it")) return LANG_ITALIAN;
  if (is("nl")) return LANG_DUTCH;
  if (is("ru")) return LANG_RUSSIAN;
  return LANG_ENGLISH;
}

HMODULE GetModuleHandleW(const wchar_t* name) {
  if (!name) return dlopen(nullptr, RTLD_NOW);
  std::string n = Narrow(name);
  for (auto& c : n) c = char(std::tolower((unsigned char)c));
  if (n.rfind("rexruntime", 0) != 0) return nullptr;
  for (const char* so : {"librexruntimerd.so", "librexruntime.so", "librexruntimed.so"})
    if (void* h = dlopen(so, RTLD_NOW | RTLD_NOLOAD)) return h;
  return nullptr;
}

void* GetProcAddress(HMODULE module, const char* name) { return module && name ? dlsym(module, name) : nullptr; }

SHORT GetAsyncKeyState(int vk) {
  SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
  if (vk >= 'A' && vk <= 'Z') sc = SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
  else if (vk >= '1' && vk <= '9') sc = SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
  else if (vk == '0') sc = SDL_SCANCODE_0;
  else if (vk >= VK_F1 && vk <= VK_F12) sc = SDL_Scancode(SDL_SCANCODE_F1 + (vk - VK_F1));
  else {
    switch (vk) {
      case VK_BACK: sc = SDL_SCANCODE_BACKSPACE; break;
      case VK_TAB: sc = SDL_SCANCODE_TAB; break;
      case VK_RETURN: sc = SDL_SCANCODE_RETURN; break;
      case VK_ESCAPE: sc = SDL_SCANCODE_ESCAPE; break;
      case VK_SPACE: sc = SDL_SCANCODE_SPACE; break;
      case VK_LEFT: sc = SDL_SCANCODE_LEFT; break;
      case VK_UP: sc = SDL_SCANCODE_UP; break;
      case VK_RIGHT: sc = SDL_SCANCODE_RIGHT; break;
      case VK_DOWN: sc = SDL_SCANCODE_DOWN; break;
      case VK_PRIOR: sc = SDL_SCANCODE_PAGEUP; break;
      case VK_NEXT: sc = SDL_SCANCODE_PAGEDOWN; break;
      case VK_HOME: sc = SDL_SCANCODE_HOME; break;
      case VK_END: sc = SDL_SCANCODE_END; break;
      case VK_DELETE: sc = SDL_SCANCODE_DELETE; break;
      case VK_LSHIFT: sc = SDL_SCANCODE_LSHIFT; break;
      case VK_RSHIFT: sc = SDL_SCANCODE_RSHIFT; break;
      case VK_LCONTROL: sc = SDL_SCANCODE_LCTRL; break;
      case VK_RCONTROL: sc = SDL_SCANCODE_RCTRL; break;
      default: break;
    }
  }
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  auto down = [&](SDL_Scancode s) { return keys && int(s) < count && keys[s]; };
  bool pressed = false;
  if (vk == VK_SHIFT) pressed = down(SDL_SCANCODE_LSHIFT) || down(SDL_SCANCODE_RSHIFT);
  else if (vk == VK_CONTROL) pressed = down(SDL_SCANCODE_LCTRL) || down(SDL_SCANCODE_RCTRL);
  else if (vk == VK_MENU) pressed = down(SDL_SCANCODE_LALT) || down(SDL_SCANCODE_RALT);
  else if (sc != SDL_SCANCODE_UNKNOWN) pressed = down(sc);
  return pressed ? SHORT(0x8000) : SHORT(0);
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* src, int n, char* dst, int dst_n, const char*, BOOL*) {
  if (!src) return 0;
  std::string out = Narrow(src, n);
  if (n < 0) out.push_back('\0');  // -1: the terminator is converted too, as on Windows
  if (!dst || dst_n == 0) return int(out.size());
  int count = std::min(int(out.size()), dst_n);
  std::memcpy(dst, out.data(), size_t(count));
  return count;
}

int MultiByteToWideChar(UINT, DWORD, const char* src, int n, wchar_t* dst, int dst_n) {
  if (!src) return 0;
  std::wstring out = Widen(n < 0 ? std::string(src) : std::string(src, size_t(n)));
  if (n < 0) out.push_back(L'\0');
  if (!dst || dst_n == 0) return int(out.size());
  int count = std::min(int(out.size()), dst_n);
  std::wmemcpy(dst, out.data(), size_t(count));
  return count;
}

void GetLocalTime(SYSTEMTIME* t) {
  if (!t) return;
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm local{};
  localtime_r(&ts.tv_sec, &local);
  *t = {WORD(local.tm_year + 1900), WORD(local.tm_mon + 1), WORD(local.tm_wday), WORD(local.tm_mday),
        WORD(local.tm_hour), WORD(local.tm_min), WORD(local.tm_sec), WORD(ts.tv_nsec / 1000000)};
}

BOOL VirtualProtect(void* address, SIZE_T size, DWORD protect, DWORD* old_protect) {
  const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
  const uintptr_t start = uintptr_t(address) & ~(page - 1);
  const uintptr_t end = (uintptr_t(address) + size + page - 1) & ~(page - 1);
  int prot = protect == PAGE_NOACCESS ? PROT_NONE : protect == PAGE_READONLY ? PROT_READ : PROT_READ | PROT_WRITE;
  if (old_protect) *old_protect = PAGE_READWRITE;  // callers restore read/write, the only state they set
  if (mprotect(reinterpret_cast<void*>(start), end - start, prot) != 0) {
    g_last_error = DWORD(errno);
    return FALSE;
  }
  return TRUE;
}

DWORD GetCurrentThreadId() { return DWORD(syscall(SYS_gettid)); }

DWORD GetLastError() { return g_last_error; }

DWORD GetTickCount() {
  using namespace std::chrono;
  return DWORD(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
