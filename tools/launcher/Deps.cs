// The Dependencies tab: everything the build needs, what is already on this PC, what is missing,
// how much space each part takes, and a button to install just that part (setup.ps1 -Only).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PortLauncher {

sealed partial class MainForm {
  sealed class Dep {
    public string Name, Detail, Only;   // Only: the setup.ps1 -Only part that installs it (null: full setup)
    public double Gb;
    public bool OnSystemDrive;          // installs on C: whatever the game folder is
    public bool? Ok;                    // null while checking
    public string Found;                // where it was found / why it is missing
  }

  TableLayoutPanel depsList;
  Label depsSummary;
  FlatButton depsAllButton, depsRecheckButton;
  readonly List<FlatButton> depButtons = new List<FlatButton>();
  List<Dep> deps = new List<Dep>();
  bool depsChecking;

  void BuildDepsPage() {
    var stack = Stack(Theme.Bg);
    stack.Controls.Add(Words(L.T("O que a montagem do jogo precisa neste PC. O que faltar pode ser instalado item por item ou tudo de uma vez.",
                                "What building the game needs on this PC. Anything missing can be installed one by one or all at once."),
                           Theme.UI(9.5f), Theme.Text3, new Padding(4, 0, 4, 14)));
    depsList = AddCard(stack, out _);
    var bottom = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = 84, ColumnCount = 3, BackColor = Theme.Bg, Padding = new Padding(0, 14, 0, 0) };
    bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
    bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
    bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
    depsSummary = new Label { AutoSize = true, MaximumSize = new Size(470, 0), ForeColor = Theme.Text2, Anchor = AnchorStyles.Left, Margin = new Padding(4, 0, 12, 0) };
    depsRecheckButton = new FlatButton(L.T("Verificar de novo", "Check again"), ButtonKind.Secondary, 170, 40) { Margin = new Padding(0, 0, 10, 0), Anchor = AnchorStyles.Right };
    depsRecheckButton.Click += (s, e) => CheckDeps();
    depsAllButton = new FlatButton(L.T("Instalar tudo o que falta", "Install everything missing"), ButtonKind.Primary, 240, 40) { Margin = Padding.Empty, Anchor = AnchorStyles.Right };
    depsAllButton.Click += (s, e) => Install();
    bottom.Controls.Add(depsSummary, 0, 0);
    bottom.Controls.Add(depsRecheckButton, 1, 0);
    bottom.Controls.Add(depsAllButton, 2, 0);
    pageDeps.Controls.Add(Scroller(stack));
    pageDeps.Controls.Add(bottom);
  }

  List<Dep> DepList() {
    var list = new List<Dep> {
      new Dep { Name = "Python 3.11+", Detail = L.T("roda os scripts de montagem e o instalador dos arquivos do jogo", "runs the build scripts and the game file installer"),
                Only = "python", Gb = cfg.PythonGb, OnSystemDrive = true },
      new Dep { Name = "Microsoft C++ Build Tools", Detail = L.T("bibliotecas C++ e o Windows SDK (pede administrador)", "C++ libraries and the Windows SDK (asks for administrator)"),
                Only = "buildtools", Gb = cfg.BuildToolsGb, OnSystemDrive = true },
    };
    foreach (var t in cfg.Tools)
      list.Add(new Dep { Name = t.Name, Detail = L.T("fica dentro da pasta do jogo (.local\\tools)", "kept inside the game folder (.local\\tools)"), Only = "tools", Gb = t.Gb });
    list.Add(new Dep { Name = L.T("Seus arquivos do jogo", "Your game files"), Detail = cfg.Disc + L.T(" (do seu disco ou .iso)", " (from your disc or .iso)"),
                       Only = "game", Gb = 0 });
    list.Add(new Dep { Name = L.T("Versão para PC (montada)", "PC version (built)"), Detail = L.T($"tradução e compilação, cerca de {cfg.Minutes} min", $"translation and compile, about {cfg.Minutes} min"),
                       Only = null, Gb = cfg.DiskGb });
    return list;
  }

  async void CheckDeps() {
    if (depsChecking) return;
    depsChecking = true;
    deps = DepList();
    RenderDeps();
    string kit = root;
    var results = await Task.Run(() => {
      var r = new List<KeyValuePair<bool, string>>();
      r.Add(FindPython());
      r.Add(FindBuildTools());
      string systemLlvm = SystemLlvm();
      foreach (var t in cfg.Tools) {
        if (t.Id == "llvm" && systemLlvm != null) { r.Add(new KeyValuePair<bool, string>(true, systemLlvm)); continue; }
        bool ok = kit != null && File.Exists(Path.Combine(kit, t.Check));
        r.Add(new KeyValuePair<bool, string>(ok, ok ? L.T("na pasta do jogo", "in the game folder") : ""));
      }
      r.Add(GameFilesOk(kit));
      r.Add(new KeyValuePair<bool, string>(Installed, Installed ? Path.GetFileName(GameExe) : ""));
      return r;
    });
    for (int i = 0; i < deps.Count && i < results.Count; ++i) { deps[i].Ok = results[i].Key; deps[i].Found = results[i].Value; }
    depsChecking = false;
    RenderDeps();
  }


  void RenderDeps() {
    depsList.SuspendLayout();
    depsList.Controls.Clear();
    depButtons.Clear();
    bool first = true;
    foreach (var d in deps) {
      if (!first) depsList.Controls.Add(new Panel { Height = 1, BackColor = Theme.Border, Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = Padding.Empty });
      first = false;
      var row = new TableLayoutPanel { ColumnCount = 5, AutoSize = true, Anchor = AnchorStyles.Left | AnchorStyles.Right, BackColor = Theme.Surface, Margin = Padding.Empty, Padding = new Padding(0, 12, 0, 12) };
      row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 44));
      row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
      row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 210));
      row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 96));
      row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
      var dot = new StatusDot { Ok = d.Ok, Anchor = AnchorStyles.Left, Margin = new Padding(0, 0, 0, 0) };
      var name = Stack(Theme.Surface);
      name.Dock = DockStyle.None; name.Anchor = AnchorStyles.Left | AnchorStyles.Right; name.Margin = new Padding(0, 0, 12, 0);
      name.Controls.Add(Words(d.Name, Theme.Semi(10.5f), Theme.Text, new Padding(0, 0, 0, 2)));
      name.Controls.Add(Words(d.Detail, Theme.UI(9f), Theme.Text3, Padding.Empty));
      string stateText = d.Ok == null ? L.T("verificando...", "checking...")
                       : d.Ok == true ? L.T("Instalado", "Installed") : L.T("Falta", "Missing");
      var state = Stack(Theme.Surface);
      state.Dock = DockStyle.None; state.Anchor = AnchorStyles.Left | AnchorStyles.Right; state.Margin = new Padding(0, 0, 8, 0);
      state.Controls.Add(Words(stateText, Theme.Semi(10f), d.Ok == null ? Theme.Text3 : d.Ok == true ? Theme.Accent : Theme.Danger, new Padding(0, 0, 0, 2)));
      if (!string.IsNullOrEmpty(d.Found)) state.Controls.Add(Words(d.Found, Theme.UI(9f), Theme.Text3, Padding.Empty));
      string sizeText = d.Gb <= 0 ? "" : d.Gb < 1 ? $"~{Math.Max(1, (int)Math.Round(d.Gb * 1024))} MB" : $"~{d.Gb:0.#} GB";
      var size = Stack(Theme.Surface);
      size.Dock = DockStyle.None; size.Anchor = AnchorStyles.Left | AnchorStyles.Right;
      size.Controls.Add(Words(sizeText, Theme.Semi(10f), Theme.Text2, new Padding(0, 0, 0, 2)));
      if (d.OnSystemDrive && sizeText.Length > 0) size.Controls.Add(Words(L.T("no C:", "on C:"), Theme.UI(9f), Theme.Text3, Padding.Empty));
      var button = new FlatButton(d.Only == null ? L.T("Montar", "Build") : L.T("Instalar", "Install"), ButtonKind.Blue, 112, 36) { Anchor = AnchorStyles.Right, Margin = Padding.Empty, Tag = d };
      button.Visible = d.Ok == false && root != null;  // before the kit is downloaded only "install everything" works
      button.Click += (s, e) => InstallDep((Dep)((Control)s).Tag);
      depButtons.Add(button);
      row.Controls.Add(dot, 0, 0);
      row.Controls.Add(name, 1, 0);
      row.Controls.Add(state, 2, 0);
      row.Controls.Add(size, 3, 0);
      row.Controls.Add(button, 4, 0);
      depsList.Controls.Add(row);
    }
    depsList.ResumeLayout();
    UpdateDepsSummary();
    UpdateDepButtons();
  }

  void UpdateDepsSummary() {
    var missing = deps.Where(d => d.Ok == false).ToList();
    double onC = missing.Where(d => d.OnSystemDrive).Sum(d => d.Gb);
    double here = missing.Where(d => !d.OnSystemDrive).Sum(d => d.Gb);
    string folder = root ?? folderBox.Text;
    string driveName = Path.GetPathRoot(folder);
    double freeHere = FreeGb(driveName), freeC = FreeGb(Path.GetPathRoot(Environment.SystemDirectory));
    bool sameDrive = string.Equals(driveName, Path.GetPathRoot(Environment.SystemDirectory), StringComparison.OrdinalIgnoreCase);
    bool shortOfSpace = false;
    string text;
    if (deps.Any(d => d.Ok == null)) text = L.T("Verificando...", "Checking...");
    else if (missing.Count == 0) text = L.T("Tudo instalado.", "Everything is installed.");
    else {
      text = L.T($"Faltam {missing.Count} itens: ~{here:0.#} GB em {driveName} (livre: {freeHere:0.#} GB)",
                 $"{missing.Count} items missing: ~{here:0.#} GB on {driveName} (free: {freeHere:0.#} GB)");
      if (onC > 0) text += sameDrive ? L.T($" + ~{onC:0.#} GB do sistema", $" + ~{onC:0.#} GB of system parts")
                                     : L.T($" e ~{onC:0.#} GB no C: (livre: {freeC:0.#} GB)", $" and ~{onC:0.#} GB on C: (free: {freeC:0.#} GB)");
      text += ".";
      shortOfSpace = sameDrive ? here + onC > freeHere : here > freeHere || onC > freeC;
      if (shortOfSpace) text += L.T(" Falta espaço em disco.", " Not enough disk space.");
    }
    depsSummary.Text = text;
    depsSummary.ForeColor = shortOfSpace ? Theme.Danger : missing.Count == 0 && !deps.Any(d => d.Ok == null) ? Theme.Accent : Theme.Text2;
  }

  static double FreeGb(string drive) {
    try { return new DriveInfo(drive).AvailableFreeSpace / (1024.0 * 1024 * 1024); } catch (Exception) { return 0; }
  }

  void UpdateDepButtons() {
    if (depsAllButton == null) return;
    bool busy = Busy;
    foreach (var b in depButtons) b.Enabled = !busy && root != null;
    depsAllButton.Enabled = !busy && deps.Any(d => d.Ok == false);
    depsRecheckButton.Enabled = !busy && !depsChecking;
  }

  void InstallDep(Dep d) {
    if (Busy || root == null) return;
    if (d.Only == null) { Install(); return; }
    ShowPage(pageInstall);
    log.Clear();
    progress.Value = 0;
    RunSetup(d.Only);
  }

  // ------------------------------------------------------------------ checks (as setup.ps1 does them)

  static string Run(string exe, string args) {
    try {
      var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
      using (var p = Process.Start(psi)) {
        string output = p.StandardOutput.ReadToEnd();
        if (!p.WaitForExit(30000)) { try { p.Kill(); } catch (Exception) { } return null; }
        return p.ExitCode == 0 ? output.Trim() : null;
      }
    } catch (Exception) { return null; }
  }

  static KeyValuePair<bool, string> FindPython() {
    const string probe = "-c \"import sys, tkinter; assert sys.version_info >= (3, 11); print('%d.%d' % sys.version_info[:2])\"";
    foreach (var c in new[] { new[] { "py", "-3 " }, new[] { "python", "-B " } }) {
      string v = Run(c[0], c[1] + probe);
      if (!string.IsNullOrEmpty(v)) return new KeyValuePair<bool, string>(true, "Python " + v);
    }
    string local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "Python", "Python312", "python.exe");
    if (File.Exists(local)) return new KeyValuePair<bool, string>(true, "Python 3.12");
    return new KeyValuePair<bool, string>(false, "");
  }

  static KeyValuePair<bool, string> FindBuildTools() {
    string x86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
    string vswhere = Path.Combine(x86, "Microsoft Visual Studio", "Installer", "vswhere.exe");
    bool sdk = Directory.Exists(Path.Combine(x86, "Windows Kits", "10", "Include"));
    string vs = File.Exists(vswhere)
        ? Run(vswhere, "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName") : null;
    if (!string.IsNullOrEmpty(vs) && sdk) return new KeyValuePair<bool, string>(true, vs.Split('\n')[0].Trim());
    if (!string.IsNullOrEmpty(vs)) return new KeyValuePair<bool, string>(false, L.T("falta o Windows SDK", "Windows SDK missing"));
    return new KeyValuePair<bool, string>(false, "");
  }

  // A complete Clang 20+ already installed is used instead of the download (setup_tools.py).
  static string SystemLlvm() {
    string bin = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "LLVM", "bin");
    string[] needed = { "clang.exe", "clang++.exe", "clang-cl.exe", "lld-link.exe", "llvm-ar.exe", "llvm-rc.exe" };
    if (!needed.All(n => File.Exists(Path.Combine(bin, n)))) return null;
    string v = Run(Path.Combine(bin, "clang++.exe"), "--version");
    var m = v == null ? null : System.Text.RegularExpressions.Regex.Match(v, @"clang version (\d+)");
    return m != null && m.Success && int.Parse(m.Groups[1].Value) >= 20 ? "Clang " + m.Groups[1].Value + L.T(" do sistema", " (system)") : null;
  }

  KeyValuePair<bool, string> GameFilesOk(string kit) {
    if (kit == null || cfg.GameFiles.Count == 0) return new KeyValuePair<bool, string>(false, "");
    foreach (var kv in cfg.GameFiles) {
      string path = Path.Combine(kit, kv.Key);
      if (!File.Exists(path)) return new KeyValuePair<bool, string>(false, "");
      using (var sha = SHA256.Create())
      using (var f = File.OpenRead(path)) {
        string hash = BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
        if (!string.Equals(hash, kv.Value, StringComparison.OrdinalIgnoreCase))
          return new KeyValuePair<bool, string>(false, L.T("versão diferente", "different version"));
      }
    }
    return new KeyValuePair<bool, string>(true, L.T("versão conferida", "version verified"));
  }
}

}
