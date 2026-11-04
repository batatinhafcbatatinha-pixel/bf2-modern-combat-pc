// What the PC settings need from Windows, enumerated the same way the game's F1 menu does it
// (pc_settings.cpp): the ini file, the monitors (primary first), their refresh rates and the
// DXGI graphics cards (the ini keeps the DXGI index; software adapters are not offered).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace PortLauncher {

static class Ini {
  [DllImport("kernel32", CharSet = CharSet.Unicode)]
  static extern int GetPrivateProfileIntW(string section, string key, int def, string file);
  [DllImport("kernel32", CharSet = CharSet.Unicode)]
  static extern bool WritePrivateProfileStringW(string section, string key, string value, string file);

  public static int Get(string file, string section, string key, int def) => GetPrivateProfileIntW(section, key, def, file);
  public static void Put(string file, string section, string key, int value) =>
      WritePrivateProfileStringW(section, key, value.ToString(System.Globalization.CultureInfo.InvariantCulture), file);
}

sealed class MonitorInfo {
  public string Device, Name;
}

static class Displays {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct MONITORINFOEX {
    public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
  }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct DISPLAY_DEVICE {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
    public uint StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
  }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public ushort dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    public uint dmFields;
    public int dmPositionX, dmPositionY;
    public uint dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public ushort dmLogPixels;
    public uint dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public uint dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
  }
  delegate bool MonitorEnumProc(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data);
  [DllImport("user32")] static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);
  [DllImport("user32", CharSet = CharSet.Unicode)] static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFOEX info);
  [DllImport("user32", CharSet = CharSet.Unicode)] static extern bool EnumDisplayDevicesW(string device, uint index, ref DISPLAY_DEVICE dd, uint flags);
  [DllImport("user32", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string device, int mode, ref DEVMODE dm);
  const int ENUM_CURRENT_SETTINGS = -1;

  public static List<MonitorInfo> Monitors() {
    var list = new List<MonitorInfo>();
    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (h, dc, r, d) => {
      var mi = new MONITORINFOEX { cbSize = Marshal.SizeOf(typeof(MONITORINFOEX)) };
      if (!GetMonitorInfoW(h, ref mi)) return true;
      int w = mi.rcMonitor.Right - mi.rcMonitor.Left, ht = mi.rcMonitor.Bottom - mi.rcMonitor.Top;
      var dd = new DISPLAY_DEVICE { cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE)) };
      string name = EnumDisplayDevicesW(mi.szDevice, 0, ref dd, 0) && !string.IsNullOrEmpty(dd.DeviceString)
          ? $"{dd.DeviceString} ({w}x{ht})" : $"{w}x{ht}";
      var m = new MonitorInfo { Device = mi.szDevice, Name = name };
      if ((mi.dwFlags & 1) != 0) list.Insert(0, m); else list.Add(m);  // primary first, as in the game
      return true;
    }, IntPtr.Zero);
    return list;
  }

  // Refresh rates the monitor offers at its current resolution, highest first.
  public static List<int> RefreshRates(string device, out int current) {
    var dm = new DEVMODE { dmSize = (ushort)Marshal.SizeOf(typeof(DEVMODE)) };
    current = EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, ref dm) && dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 60;
    uint w = dm.dmPelsWidth, h = dm.dmPelsHeight;
    var rates = new SortedSet<int>();
    for (int i = 0; ; ++i) {
      var m = new DEVMODE { dmSize = (ushort)Marshal.SizeOf(typeof(DEVMODE)) };
      if (!EnumDisplaySettingsW(device, i, ref m)) break;
      if (m.dmPelsWidth == w && m.dmPelsHeight == h && m.dmDisplayFrequency > 1) rates.Add((int)m.dmDisplayFrequency);
    }
    return rates.Reverse().ToList();
  }
}

static class Gpus {
  [ComImport, Guid("770aae78-f26f-4dba-a829-253c83d1b387"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IDXGIFactory1 {
    void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
    void EnumAdapters(); void MakeWindowAssociation(); void GetWindowAssociation(); void CreateSwapChain(); void CreateSoftwareAdapter();
    [PreserveSig] int EnumAdapters1(uint index, out IDXGIAdapter1 adapter);
    [PreserveSig] bool IsCurrent();
  }
  [ComImport, Guid("29038f61-3839-4626-91fd-086879011a05"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IDXGIAdapter1 {
    void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
    void EnumOutputs(); void GetDesc(); void CheckInterfaceSupport();
    [PreserveSig] int GetDesc1(out DXGI_ADAPTER_DESC1 desc);
  }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct DXGI_ADAPTER_DESC1 {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
    public uint VendorId, DeviceId, SubSysId, Revision;
    public UIntPtr DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
    public uint LuidLow; public int LuidHigh;
    public uint Flags;
  }
  [DllImport("dxgi")] static extern int CreateDXGIFactory1(ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object factory);

  // (DXGI index, name) of every hardware adapter.
  public static List<KeyValuePair<int, string>> List() {
    var list = new List<KeyValuePair<int, string>>();
    try {
      var iid = typeof(IDXGIFactory1).GUID;
      if (CreateDXGIFactory1(ref iid, out object o) < 0) return list;
      var f = (IDXGIFactory1)o;
      for (uint i = 0; f.EnumAdapters1(i, out var a) >= 0; ++i) {
        if (a.GetDesc1(out var d) >= 0 && (d.Flags & 2) == 0) list.Add(new KeyValuePair<int, string>((int)i, d.Description));
        Marshal.ReleaseComObject(a);
      }
      Marshal.ReleaseComObject(f);
    } catch (Exception) { }
    return list;
  }
}

}
