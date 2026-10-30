#pragma once
// The Win32 subset the PC settings screen uses, for Linux builds (DXVK native provides the basic types).
// GDI drawing goes to the same 32-bit top-down DIB the Windows build uses: colours are written as 0x00RRGGBB
// and antialiased text is blended over the pixel, leaving the top byte for the caller to set, as GDI does.
#ifndef _WIN32
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstdlib>

using HGDIOBJ = void*;
using HFONT = HGDIOBJ;
using HBRUSH = HGDIOBJ;
using HPEN = HGDIOBJ;
using HBITMAP = HGDIOBJ;

struct BITMAPINFOHEADER {
  DWORD biSize;
  LONG biWidth;
  LONG biHeight;
  WORD biPlanes;
  WORD biBitCount;
  DWORD biCompression;
  DWORD biSizeImage;
  LONG biXPelsPerMeter;
  LONG biYPelsPerMeter;
  DWORD biClrUsed;
  DWORD biClrImportant;
};
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };

#ifndef BI_RGB
#define BI_RGB 0
#endif
#define DIB_RGB_COLORS 0
#define TRANSPARENT 1
#define OPAQUE 2
#define PS_SOLID 0
#define FW_NORMAL 400
#define FW_BOLD 700
#define FW_HEAVY 900
#define DEFAULT_CHARSET 1
#define OUT_TT_PRECIS 4
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define DEFAULT_PITCH 0
#define LOGPIXELSY 90
#define DT_LEFT 0x0000
#define DT_CENTER 0x0001
#define DT_RIGHT 0x0002
#define DT_VCENTER 0x0004
#define DT_SINGLELINE 0x0020
#define DT_NOPREFIX 0x0800
#define DT_END_ELLIPSIS 0x8000
#define MAP_COMPOSITE 0x40

#define VK_BACK 0x08
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_TAB 0x09
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0
#endif
#define ERROR_DEVICE_NOT_CONNECTED 1167

#define LANG_DUTCH 0x13
#define LANG_ENGLISH 0x09
#define LANG_FRENCH 0x0c
#define LANG_GERMAN 0x07
#define LANG_ITALIAN 0x10
#define LANG_PORTUGUESE 0x16
#define LANG_RUSSIAN 0x19
#define LANG_SPANISH 0x0a
#ifndef PRIMARYLANGID
#define PRIMARYLANGID(l) (WORD(l) & 0x3ff)
#endif

struct STARTUPINFOW { DWORD cb; };
struct PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; };

// GDI
HDC CreateCompatibleDC(HDC);
BOOL DeleteDC(HDC);
HBITMAP CreateDIBSection(HDC, const BITMAPINFO*, UINT, void** bits, HANDLE, DWORD);
HGDIOBJ SelectObject(HDC, HGDIOBJ);
BOOL DeleteObject(HGDIOBJ);
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality,
                  DWORD pitch, const wchar_t* face);
COLORREF SetTextColor(HDC, COLORREF);
int SetBkMode(HDC, int);
int DrawTextW(HDC, const wchar_t* text, int count, RECT* rect, UINT format);
BOOL GetTextExtentPoint32W(HDC, const wchar_t* text, int count, SIZE* size);
HBRUSH CreateSolidBrush(COLORREF);
HPEN CreatePen(int style, int width, COLORREF);
int FillRect(HDC, const RECT*, HBRUSH);
BOOL Polygon(HDC, const POINT*, int count);
BOOL Ellipse(HDC, int left, int top, int right, int bottom);
inline BOOL GdiFlush() { return TRUE; }
#include <strings.h>
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline HDC GetDC(HWND) { return nullptr; }
inline int ReleaseDC(HWND, HDC) { return 1; }
inline int GetDeviceCaps(HDC, int) { return 96; }
inline BOOL SetRect(RECT* r, int l, int t, int rt, int b) {
  if (!r) return FALSE;
  r->left = l; r->top = t; r->right = rt; r->bottom = b;
  return TRUE;
}
inline DWORD CharUpperBuffW(wchar_t* s, DWORD n) {
  for (DWORD i = 0; i < n; ++i) s[i] = wchar_t(std::towupper(wint_t(s[i])));
  return n;
}
int FoldStringW(DWORD flags, const wchar_t* src, int n, wchar_t* dst, int dst_n);

// Windows and input (the window handle is the SDL3 window, as DXVK native's SDL3 WSI expects)
BOOL GetCursorPos(POINT*);
BOOL ScreenToClient(HWND, POINT*);
HWND GetForegroundWindow();
BOOL IsWindowVisible(HWND);
BOOL IsIconic(HWND);

// Process, environment, files
DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size);
BOOL SetEnvironmentVariableA(const char* name, const char* value);
UINT GetPrivateProfileIntW(const wchar_t* section, const wchar_t* key, int def, const wchar_t* file);
BOOL WritePrivateProfileStringW(const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* file);
DWORD GetModuleFileNameW(HMODULE, wchar_t* out, DWORD n);
DWORD GetModuleFileNameA(HMODULE, char* out, DWORD n);
const wchar_t* GetCommandLineW();
BOOL CreateProcessW(const wchar_t* app, const wchar_t* cmdline, void*, void*, BOOL, DWORD, void*, const wchar_t*,
                    STARTUPINFOW*, PROCESS_INFORMATION*);
[[noreturn]] void ExitProcess(UINT code);
WORD GetUserDefaultUILanguage();
DWORD GetLastError();
DWORD GetTickCount();
void Sleep(DWORD ms);
// No XInput or DXVK DLLs to load on Linux: pads come through the SDK's SDL input, D3D11 is DXVK native.
inline HMODULE LoadLibraryW(const wchar_t*) { return nullptr; }
// rexruntime*.dll -> the SDK runtime shared library already loaded in the process; other modules: none.
HMODULE GetModuleHandleW(const wchar_t* name);
void* GetProcAddress(HMODULE module, const char* name);
// Keyboard state of a Windows virtual key (bit 15 = down), from SDL's keyboard state.
SHORT GetAsyncKeyState(int vk);
#ifndef E_UNEXPECTED
#define E_UNEXPECTED ((HRESULT)0x8000FFFFL)
#endif
#define CP_UTF8 65001
#define CP_ACP 0
int WideCharToMultiByte(UINT codepage, DWORD flags, const wchar_t* src, int n, char* dst, int dst_n, const char*, BOOL*);
int MultiByteToWideChar(UINT codepage, DWORD flags, const char* src, int n, wchar_t* dst, int dst_n);
#define sscanf_s sscanf
inline unsigned long long _strtoui64(const char* s, char** end, int base) { return std::strtoull(s, end, base); }
inline long long _strtoi64(const char* s, char** end, int base) { return std::strtoll(s, end, base); }
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
void GetLocalTime(SYSTEMTIME* t);
#pragma pack(push, 2)
struct BITMAPFILEHEADER { WORD bfType; DWORD bfSize; WORD bfReserved1; WORD bfReserved2; DWORD bfOffBits; };
#pragma pack(pop)
#ifndef D3D11_FILTER_REDUCTION_TYPE_STANDARD
#define D3D11_FILTER_REDUCTION_TYPE_STANDARD 0  // DXVK native's D3D11_ENCODE_BASIC_FILTER takes the comparison flag
#endif
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
// mprotect on the page range (addresses page-aligned as on Windows); returns FALSE on failure.
BOOL VirtualProtect(void* address, SIZE_T size, DWORD protect, DWORD* old_protect);
DWORD GetCurrentThreadId();
using MMRESULT = UINT;
inline MMRESULT timeBeginPeriod(UINT) { return 0; }  // POSIX sleeps are already fine-grained
inline MMRESULT timeEndPeriod(UINT) { return 0; }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
#ifndef CloseHandle
inline BOOL NativeCloseHandle(HANDLE) { return TRUE; }
#define CloseHandle NativeCloseHandle
#endif
#endif
