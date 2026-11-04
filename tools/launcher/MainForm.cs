// The launcher window: Play (status, install / repair, play), Dependencies, Install (the setup log) and
// PC settings (the game's ini, the same file the in-game F1 menu uses). Drawn with Theme's controls.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PortLauncher {

sealed partial class MainForm : Form {
  readonly GameConfig cfg;
  string root;                // the kit folder (setup.json, tools\setup.ps1); null before download
  readonly string launcherPath = Application.ExecutablePath;

  // Set when the theme changes: Program opens the window again in the new colours.
  public bool Restart;
  public string RootForRestart => root;
  public int PageForRestart => pages.IndexOf(current);

  readonly Panel host = new Panel();
  readonly List<Panel> pages = new List<Panel>();
  readonly List<NavTab> navTabs = new List<NavTab>();
  Panel current;
  Panel pagePlay, pageDeps, pageInstall, pageSettings;

  Label status, statusDetail, stepLabel, settingsNote, saveState;
  FlatButton playButton, installButton, saveButton, defaultsButton, cancelButton;
  TextBox log, folderBox;
  Control folderRow;
  ToggleSwitch shortcutBox;
  ProgressLine progress;
  readonly Dictionary<Setting, Control> editors = new Dictionary<Setting, Control>();
  readonly Dictionary<Setting, Control> rows = new Dictionary<Setting, Control>();
  ChoiceBox monitorCombo, refreshCombo;
  Process running;
  bool dirty, loading;

  public MainForm(GameConfig config, string kitDir, int page = 0) {
    cfg = config;
    root = kitDir;
    Text = cfg.Game + " - PC";
    try { Icon = Icon.ExtractAssociatedIcon(launcherPath); } catch (Exception) { }
    Font = Theme.UI(10f);
    AutoScaleMode = AutoScaleMode.Dpi;
    ClientSize = new Size(980, 660);
    MinimumSize = new Size(840, 600);
    StartPosition = FormStartPosition.CenterScreen;
    BackColor = Theme.Bg;
    ForeColor = Theme.Text;
    Theme.DarkTitle(this);

    // Header: title, subtitle, theme picker; the page tabs along its bottom edge.
    var header = new Panel { Dock = DockStyle.Top, Height = 124, BackColor = Theme.Surface };
    var title = new Label { Text = cfg.Game.ToUpperInvariant(), Font = Theme.Semi(21f), ForeColor = Theme.Text, AutoSize = true, Location = new Point(26, 14) };
    var sub = new Label {
      Text = L.T("Port para PC, montado a partir do seu próprio disco", "PC port, built from your own disc") +
             (string.IsNullOrEmpty(cfg.Disc) ? "" : "   ·   " + cfg.Disc),
      ForeColor = Theme.Text3, AutoSize = true, Location = new Point(29, 56) };
    var themeLabel = new Label { Text = L.T("Tema", "Theme"), ForeColor = Theme.Text3, AutoSize = true, Anchor = AnchorStyles.Top | AnchorStyles.Right };
    var themeBox = new ChoiceBox { Width = 170, Height = 32, Anchor = AnchorStyles.Top | AnchorStyles.Right };
    themeBox.Items.Add(new ChoiceBox.Entry { Value = 0, Text = L.T("Igual ao sistema", "Same as Windows") });
    themeBox.Items.Add(new ChoiceBox.Entry { Value = 1, Text = L.T("Escuro", "Dark") });
    themeBox.Items.Add(new ChoiceBox.Entry { Value = 2, Text = L.T("Claro", "Light") });
    themeBox.Select(Theme.Mode == "dark" ? 1 : Theme.Mode == "light" ? 2 : 0, 0);
    themeBox.SelectedIndexChanged += (s, e) => {
      if (Busy) { Dialog.Info(this, Text, L.T("Espere terminar para trocar o tema.", "Wait for it to finish before changing the theme.")); return; }
      Theme.Apply(themeBox.Selected.Value == 1 ? "dark" : themeBox.Selected.Value == 2 ? "light" : "system");
      Theme.Save();
      if (dirty && root != null) SaveSettings();
      Restart = true;
      Close();
    };
    header.Controls.Add(title);
    header.Controls.Add(sub);
    header.Controls.Add(themeLabel);
    header.Controls.Add(themeBox);
    header.Resize += (s, e) => {
      themeBox.Location = new Point(header.ClientSize.Width - themeBox.Width - 26, 22);
      themeLabel.Location = new Point(themeBox.Left - themeLabel.PreferredWidth - 10, 29);
    };
    var nav = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 44, BackColor = Theme.Surface, Padding = new Padding(18, 0, 0, 0), WrapContents = false };
    header.Controls.Add(nav);
    var line = new Panel { Dock = DockStyle.Top, Height = 1, BackColor = Theme.Border };

    host.Dock = DockStyle.Fill;
    host.BackColor = Theme.Bg;
    pagePlay = AddPage(nav, L.T("Jogar", "Play"));
    pageDeps = AddPage(nav, L.T("Dependências", "Dependencies"));
    pageInstall = AddPage(nav, L.T("Instalação", "Install"));
    pageSettings = AddPage(nav, L.T("Configurações do PC", "PC settings"));
    BuildPlayPage();
    BuildDepsPage();
    BuildInstallPage();
    BuildSettingsPage();

    Controls.Add(host);
    Controls.Add(line);
    Controls.Add(header);
    FormClosing += OnClosing;
    Shown += (s, e) => { ActiveControl = null; folderBox.SelectionLength = 0; };
    ShowPage(pages[Math.Max(0, Math.Min(pages.Count - 1, page))]);
    RefreshStatus();
  }

  Panel AddPage(FlowLayoutPanel nav, string name) {
    var page = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Bg, Padding = new Padding(28, 22, 28, 20), Visible = false };
    host.Controls.Add(page);
    pages.Add(page);
    var tab = new NavTab(name) { Tag = page };
    tab.Click += (s, e) => ShowPage(page);
    navTabs.Add(tab);
    nav.Controls.Add(tab);
    return page;
  }

  void ShowPage(Panel page) {
    if (current == page) return;
    current = page;
    foreach (var p in pages) p.Visible = p == page;
    foreach (var t in navTabs) t.Selected = t.Tag == page;
    if (page == pageSettings) LoadSettings();
    if (page == pageDeps) CheckDeps();
  }

  public void ShotOnLoad(string file, int tab) {
    Shown += (o, e) => {
      ShowPage(pages[Math.Max(0, Math.Min(pages.Count - 1, tab))]);
      var timer = new Timer { Interval = 5000 };
      timer.Tick += (t, a) => {
        timer.Stop();
        using (var bmp = new Bitmap(Width, Height)) {
          DrawToBitmap(bmp, new Rectangle(0, 0, Width, Height));
          bmp.Save(file, System.Drawing.Imaging.ImageFormat.Png);
        }
        Close();
      };
      timer.Start();
    };
  }

  // "owner/fifa-street-3-pc" -> "fifa-street-3-pc": the folder name.
  string RepoName => string.IsNullOrEmpty(cfg.Repo) ? "game" : cfg.Repo.Substring(cfg.Repo.LastIndexOf('/') + 1);

  public static bool IsKit(string dir) => dir != null && File.Exists(Path.Combine(dir, "setup.json")) && File.Exists(Path.Combine(dir, "tools", "setup.ps1"));
  string GameExe => root == null || cfg.Exe == null ? null : Path.Combine(root, cfg.Exe);
  string IniPath => root == null || cfg.Ini == null ? null : Path.Combine(root, cfg.Ini);
  bool Installed => GameExe != null && File.Exists(GameExe);
  bool Busy => running != null && !running.HasExited;
  bool SetupRunning => Busy && running.StartInfo.FileName.EndsWith("powershell.exe", StringComparison.OrdinalIgnoreCase);

  // ------------------------------------------------------------------ layout helpers

  // A vertical stack that fills its width; children with Anchor Left|Right stretch, labels wrap.
  static TableLayoutPanel Stack(Color back) {
    var t = new TableLayoutPanel { ColumnCount = 1, Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = back, Margin = Padding.Empty, Padding = Padding.Empty };
    t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
    return t;
  }

  static Label Words(string text, Font font, Color color, Padding margin) =>
      new Label { Text = text, Font = font, ForeColor = color, AutoSize = true, Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = margin };

  // A card holding a stack; returns the stack to fill.
  static TableLayoutPanel AddCard(TableLayoutPanel parent, out Card card, Padding? margin = null) {
    card = new Card { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = margin ?? new Padding(0, 0, 0, 16) };
    var inner = Stack(Theme.Surface);
    card.Controls.Add(inner);
    parent.Controls.Add(card);
    return inner;
  }

  static Panel Scroller(TableLayoutPanel content) {
    var p = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Theme.Bg };
    p.Controls.Add(content);
    Theme.DarkScroll(p);
    return p;
  }

  static FlowLayoutPanel Row(Color back, Padding margin) =>
      new FlowLayoutPanel { AutoSize = true, WrapContents = false, BackColor = back, Margin = margin, Padding = Padding.Empty };

  // ------------------------------------------------------------------ Play page

  void BuildPlayPage() {
    var stack = Stack(Theme.Bg);
    var main = AddCard(stack, out _);
    status = Words("", Theme.Semi(16f), Theme.Text, new Padding(0, 0, 0, 4));
    statusDetail = Words("", Theme.UI(10f), Theme.Text2, new Padding(0, 0, 0, 16));
    main.Controls.Add(status);
    main.Controls.Add(statusDetail);

    // Install folder (only before the kit is downloaded).
    var folder = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Anchor = AnchorStyles.Left | AnchorStyles.Right, BackColor = Theme.Surface, Margin = new Padding(0, 0, 0, 16) };
    folder.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
    folder.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
    var folderLabel = Words(L.T("Pasta de instalação", "Install folder"), Theme.Semi(9.5f), Theme.Text3, new Padding(0, 0, 0, 6));
    folder.Controls.Add(folderLabel, 0, 0);
    folder.SetColumnSpan(folderLabel, 2);
    var field = new Card { BackColor = Theme.Surface2, Height = 38, Padding = new Padding(12, 9, 12, 6), Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = new Padding(0, 0, 10, 0) };
    folderBox = new TextBox { BorderStyle = BorderStyle.None, BackColor = Theme.Surface2, ForeColor = Theme.Text, Dock = DockStyle.Fill, Font = Theme.UI(10f) };
    field.Controls.Add(folderBox);
    folder.Controls.Add(field, 0, 1);
    var browse = new FlatButton(L.T("Escolher...", "Browse..."), ButtonKind.Secondary, 120, 38) { Margin = Padding.Empty };
    browse.Click += (s, e) => {
      using (var d = new FolderBrowserDialog { Description = L.T("Onde instalar o jogo", "Where to install the game"), SelectedPath = folderBox.Text }) {
        if (d.ShowDialog(this) == DialogResult.OK) folderBox.Text = Path.Combine(d.SelectedPath, RepoName);
      }
    };
    folder.Controls.Add(browse, 1, 1);
    folderRow = folder;
    main.Controls.Add(folder);

    var buttons = Row(Theme.Surface, new Padding(0, 0, 0, 12));
    playButton = new FlatButton(L.T("JOGAR", "PLAY"), ButtonKind.Primary, 210, 50) { Font = Theme.Semi(13f), Margin = new Padding(0, 0, 12, 0) };
    playButton.Click += (s, e) => Play();
    installButton = new FlatButton(L.T("INSTALAR", "INSTALL"), ButtonKind.Blue, 230, 50) { Font = Theme.Semi(11f), Margin = Padding.Empty };
    installButton.Click += (s, e) => Install();
    buttons.Controls.Add(playButton);
    buttons.Controls.Add(installButton);
    main.Controls.Add(buttons);

    shortcutBox = new ToggleSwitch { Checked = true, Width = 420, Margin = new Padding(0, 4, 0, 6) };
    shortcutBox.OnText = shortcutBox.OffText = L.T("Criar atalho na área de trabalho", "Create a desktop shortcut");
    main.Controls.Add(shortcutBox);

    var more = AddCard(stack, out _);
    more.Controls.Add(Words(L.T("ATALHOS", "SHORTCUTS"), Theme.Semi(9.5f), Theme.Text3, new Padding(0, 0, 0, 8)));
    var links = Row(Theme.Surface, Padding.Empty);
    links.Controls.Add(Ghost(L.T("Abrir a pasta do jogo", "Open the game folder"), () => { if (root != null) Process.Start("explorer.exe", "\"" + root + "\""); }));
    links.Controls.Add(Ghost(L.T("Ver dependências", "See dependencies"), () => ShowPage(pageDeps)));
    links.Controls.Add(Ghost(L.T("Log da instalação", "Install log"), () => ShowPage(pageInstall)));
    links.Controls.Add(Ghost(L.T("Configurações do PC", "PC settings"), () => ShowPage(pageSettings)));
    more.Controls.Add(links);

    stack.Controls.Add(Words(L.T("Seu jogo nunca é enviado para lugar nenhum: tudo é montado neste PC a partir do seu disco. " +
                                "No jogo, F1 (ou Back+Start no controle) abre as mesmas configurações.",
                                "Your game is never uploaded anywhere: everything is built on this PC from your disc. " +
                                "In the game, F1 (or Back+Start on a controller) opens the same settings."),
                           Theme.UI(9.5f), Theme.Text3, new Padding(4, 0, 4, 0)));
    pagePlay.Controls.Add(Scroller(stack));
    folderBox.Text = DefaultInstallFolder();
  }

  FlatButton Ghost(string text, Action action) {
    var b = new FlatButton(text, ButtonKind.Secondary, TextRenderer.MeasureText(text, Theme.Semi(10f)).Width + 32, 36) { Font = Theme.Semi(10f), Margin = new Padding(0, 0, 10, 0) };
    b.Click += (s, e) => action();
    return b;
  }

  // The fixed drive with the most free space (the build needs several GB).
  string DefaultInstallFolder() {
    var best = DriveInfo.GetDrives().Where(d => d.DriveType == DriveType.Fixed && d.IsReady)
        .OrderByDescending(d => d.AvailableFreeSpace).FirstOrDefault();
    return Path.Combine(best?.RootDirectory.FullName ?? "C:\\", "Games", RepoName);
  }

  void RefreshStatus() {
    bool busy = Busy;
    folderRow.Visible = root == null;
    if (SetupRunning) {
      status.Text = L.T("Instalando...", "Installing..."); status.ForeColor = Theme.Text;
      statusDetail.Text = L.T("Acompanhe na aba Instalação. Pode demorar; deixe esta janela aberta.", "Follow it on the Install tab. It takes a while; keep this window open.");
    } else if (busy) {
      status.Text = L.T("Em jogo", "Playing"); status.ForeColor = Theme.Accent;
      statusDetail.Text = L.T("O launcher volta quando o jogo fechar.", "The launcher comes back when the game closes.");
    } else if (root == null) {
      status.Text = L.T("Não instalado", "Not installed"); status.ForeColor = Theme.Text;
      statusDetail.Text = L.T(
          $"O instalador baixa o kit do port (código-fonte, sem nada do jogo), instala as ferramentas de compilação, pede o seu disco ({cfg.Disc}) e monta a versão para PC neste computador. " +
          $"Precisa de cerca de {cfg.DiskGb} GB livres e {cfg.Minutes} minutos.",
          $"The installer downloads the port kit (source code, nothing from the game), installs the build tools, asks for your disc ({cfg.Disc}) and builds the PC version on this computer. " +
          $"About {cfg.DiskGb} GB free and {cfg.Minutes} minutes are needed.");
    } else if (Installed) {
      status.Text = L.T("Pronto para jogar", "Ready to play"); status.ForeColor = Theme.Accent;
      statusDetail.Text = root;
    } else {
      status.Text = L.T("Instalação incompleta", "Not built yet"); status.ForeColor = Theme.Text;
      statusDetail.Text = L.T($"O kit está em {root}. Clique em INSTALAR para instalar as ferramentas, escolher o seu disco e montar o jogo (cerca de {cfg.Minutes} minutos).",
                              $"The kit is in {root}. Click INSTALL to get the build tools, choose your disc and build the game (about {cfg.Minutes} minutes).");
    }
    playButton.Enabled = Installed && !busy;
    installButton.Enabled = !busy;
    installButton.Text = Installed ? L.T("REPARAR / ATUALIZAR", "REPAIR / UPDATE") : L.T("INSTALAR", "INSTALL");
    installButton.Kind = Installed ? ButtonKind.Secondary : ButtonKind.Blue;
    installButton.Invalidate();
    shortcutBox.Visible = !Installed;
    cancelButton.Enabled = SetupRunning;
    UpdateDepButtons();
    if (!busy && deps.Count > 0 && current == pageDeps) CheckDeps();
  }

  // ------------------------------------------------------------------ Install page

  void BuildInstallPage() {
    var top = Stack(Theme.Bg);
    var card = AddCard(top, out _);
    stepLabel = Words(L.T("Nada em andamento.", "Nothing running."), Theme.Semi(11.5f), Theme.Text, new Padding(0, 0, 0, 10));
    progress = new ProgressLine { Anchor = AnchorStyles.Left | AnchorStyles.Right, Height = 6, Maximum = 5, Margin = Padding.Empty };
    card.Controls.Add(stepLabel);
    card.Controls.Add(progress);

    var logCard = new Card { Dock = DockStyle.Fill, Padding = new Padding(14, 12, 6, 12) };
    log = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, BorderStyle = BorderStyle.None,
                        BackColor = Theme.Surface, ForeColor = Theme.Text2, Font = new Font("Consolas", 9.5f) };
    Theme.DarkScroll(log);
    logCard.Controls.Add(log);
    var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 56, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(0, 14, 0, 0), BackColor = Theme.Bg };
    cancelButton = new FlatButton(L.T("Parar", "Stop"), ButtonKind.Danger, 130, 38) { Margin = Padding.Empty };
    cancelButton.Click += (s, e) => StopInstall();
    bottom.Controls.Add(cancelButton);
    pageInstall.Controls.Add(logCard);
    pageInstall.Controls.Add(top);
    pageInstall.Controls.Add(bottom);
  }

  void AppendLog(string line) {
    if (line == null) return;
    if (InvokeRequired) { BeginInvoke(new Action<string>(AppendLog), line); return; }
    log.AppendText(line + Environment.NewLine);
    var m = Regex.Match(line, @"^== (\d)/(\d) (.*)$");
    if (m.Success) {
      progress.Maximum = int.Parse(m.Groups[2].Value);
      progress.Value = int.Parse(m.Groups[1].Value) - 1;
      stepLabel.Text = m.Groups[1].Value + "/" + m.Groups[2].Value + "   " + m.Groups[3].Value;
    }
  }

  async void Install() {
    if (Busy) return;
    string target = root;
    if (target == null) {
      target = folderBox.Text.Trim();
      if (target.Length == 0 || !Path.IsPathRooted(target)) {
        Dialog.Info(this, Text, L.T("Escolha uma pasta de instalação.", "Choose an install folder.")); return;
      }
    }
    string msg = L.T(
        "O instalador vai:\n\n" +
        (root == null ? "•  baixar o kit do port do GitHub (só código-fonte, cerca de 1 MB)\n" : "") +
        "•  instalar o Python e o Microsoft C++ Build Tools se faltarem (o Build Tools tem cerca de 7 GB e pede permissão de administrador)\n" +
        "•  baixar as ferramentas de compilação para dentro da pasta do jogo\n" +
        $"•  pedir o seu disco ({cfg.Disc}) ou a imagem .iso\n" +
        $"•  montar a versão para PC (cerca de {cfg.Minutes} minutos)\n\n" +
        $"Pasta: {target}",
        "The installer will:\n\n" +
        (root == null ? "•  download the port kit from GitHub (source code only, about 1 MB)\n" : "") +
        "•  install Python and the Microsoft C++ Build Tools if missing (the Build Tools are about 7 GB and ask for administrator permission)\n" +
        "•  download the build tools into the game folder\n" +
        $"•  ask for your disc ({cfg.Disc}) or its .iso image\n" +
        $"•  build the PC version (about {cfg.Minutes} minutes)\n\n" +
        $"Folder: {target}");
    if (!Dialog.Ask(this, Text, msg, L.T("Instalar", "Install"), L.T("Cancelar", "Cancel"))) return;

    ShowPage(pageInstall);
    log.Clear();
    progress.Value = 0;
    installButton.Enabled = false;
    try {
      if (root == null) {
        stepLabel.Text = L.T("Baixando o kit do port...", "Downloading the port kit...");
        root = await Task.Run(() => KitDownload.Fetch(cfg, target, AppendLog));
      }
    } catch (Exception ex) {
      AppendLog(ex.Message);
      stepLabel.Text = L.T("O download do kit falhou.", "Downloading the kit failed.");
      RefreshStatus();
      return;
    }
    RunSetup();
  }

  static readonly Encoding Oem = Encoding.GetEncoding(System.Globalization.CultureInfo.CurrentCulture.TextInfo.OEMCodePage);

  void RunSetup(string only = null) {
    var psi = new ProcessStartInfo {
      FileName = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"),
      Arguments = $"-NoProfile -ExecutionPolicy Bypass -File \"{Path.Combine(root, cfg.Setup)}\" -Yes -NoShortcut" + (only != null ? " -Only " + only : ""),
      WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
      RedirectStandardOutput = true, RedirectStandardError = true,
      StandardOutputEncoding = Oem, StandardErrorEncoding = Oem,
    };
    psi.EnvironmentVariables["PORT_LAUNCHER"] = "1";
    // PowerShell writes the console's OEM code page into the pipe; Python is told to do the same.
    psi.EnvironmentVariables["PYTHONIOENCODING"] = "cp" + Oem.CodePage + ":replace";
    var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
    p.OutputDataReceived += (s, e) => AppendLog(e.Data);
    p.ErrorDataReceived += (s, e) => AppendLog(e.Data);
    p.Exited += (s, e) => BeginInvoke(new Action(() => SetupFinished(p.ExitCode, only)));
    running = p;
    stepLabel.Text = L.T("Preparando...", "Starting...");
    p.Start();
    p.BeginOutputReadLine();
    p.BeginErrorReadLine();
    RefreshStatus();
  }

  void SetupFinished(int code, string only) {
    running = null;
    if (only != null) {
      stepLabel.Text = code == 0 ? L.T("Pronto.", "Done.") : L.T("Parou com erro. Veja o log abaixo (também em setup-log.txt).", "Stopped with an error. See the log below (also in setup-log.txt).");
      if (code == 0) ShowPage(pageDeps);
      RefreshStatus();
      return;
    }
    if (code == 0 && Installed) {
      progress.Value = progress.Maximum;
      stepLabel.Text = L.T("Pronto! O jogo está instalado.", "Done! The game is installed.");
      if (shortcutBox.Checked) CreateShortcut();
      ShowPage(pagePlay);
    } else {
      stepLabel.Text = L.T("A instalação parou. Veja o log abaixo (também em setup-log.txt) e clique em INSTALAR de novo: as etapas prontas são puladas.",
                           "Setup stopped. See the log below (also in setup-log.txt) and click INSTALL again: finished steps are skipped.");
    }
    RefreshStatus();
  }

  void StopInstall() {
    if (!Busy) return;
    if (!Dialog.Ask(this, Text, L.T("Parar agora? Dá para continuar depois: as etapas prontas são puladas.", "Stop now? You can continue later: finished steps are skipped."),
                    L.T("Parar", "Stop"), L.T("Continuar", "Keep going"), true)) return;
    KillTree(running.Id);
  }

  static void KillTree(int pid) {
    try { Process.Start(new ProcessStartInfo("taskkill", $"/T /F /PID {pid}") { CreateNoWindow = true, UseShellExecute = false })?.WaitForExit(); }
    catch (Exception) { }
  }

  // The launcher itself is the shortcut target: it plays and keeps the settings at hand.
  void CreateShortcut() {
    try {
      string exe = Path.Combine(root, Path.GetFileName(launcherPath));
      if (!File.Exists(exe)) File.Copy(launcherPath, exe);
      var shellType = Type.GetTypeFromProgID("WScript.Shell");
      dynamic shell = Activator.CreateInstance(shellType);
      dynamic link = shell.CreateShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), cfg.Game + ".lnk"));
      link.TargetPath = exe;
      link.WorkingDirectory = root;
      link.IconLocation = exe + ",0";
      link.Save();
    } catch (Exception ex) {
      AppendLog("Shortcut: " + ex.Message);
    }
  }

  void OnClosing(object sender, FormClosingEventArgs e) {
    if (dirty && root != null) SaveSettings();
    if (SetupRunning) {
      if (!Dialog.Ask(this, Text, L.T("A instalação ainda está rodando. Parar e fechar?", "Setup is still running. Stop it and close?"),
                      L.T("Parar e fechar", "Stop and close"), L.T("Voltar", "Back"), true)) { e.Cancel = true; Restart = false; return; }
      KillTree(running.Id);
    }
  }

  // ------------------------------------------------------------------ Play

  void Play() {
    if (!Installed || Busy) return;
    if (dirty) SaveSettings();
    var output = new StringBuilder();
    var psi = new ProcessStartInfo {
      FileName = Path.Combine(Environment.SystemDirectory, "cmd.exe"),
      Arguments = $"/c \"\"{Path.Combine(root, cfg.Play)}\" < nul\"",
      WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
      RedirectStandardOutput = true, RedirectStandardError = true,
    };
    var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
    p.OutputDataReceived += (s, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
    p.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
    p.Exited += (s, e) => BeginInvoke(new Action(() => {
      running = null;
      WindowState = FormWindowState.Normal;
      Activate();
      if (p.ExitCode != 0) {
        string text;
        lock (output) text = output.ToString().Trim();
        if (text.Length > 2000) text = "..." + text.Substring(text.Length - 2000);
        Dialog.Info(this, Text, L.T("O jogo fechou com erro (código ", "The game closed with an error (code ") + p.ExitCode + ").\n\n" + text);
      }
      RefreshStatus();
    }));
    running = p;
    p.Start();
    p.BeginOutputReadLine();
    p.BeginErrorReadLine();
    WindowState = FormWindowState.Minimized;
    RefreshStatus();
  }

  // ------------------------------------------------------------------ PC settings page

  static readonly HashSet<string> OffWords = new HashSet<string> { "Desligada", "Desligado", "Não", "Esconder", "Off", "No", "Hide" };

  void BuildSettingsPage() {
    var stack = Stack(Theme.Bg);
    settingsNote = Words("", Theme.UI(9.5f), Theme.Text3, new Padding(4, 0, 4, 14));
    stack.Controls.Add(settingsNote);

    var monitors = Displays.Monitors();
    var gpus = Gpus.List();
    foreach (var g in cfg.Groups) {
      var card = AddCard(stack, out _);
      card.Controls.Add(Words(L.T(g).ToUpperInvariant(), Theme.Semi(9.5f), Theme.Accent, new Padding(0, 0, 0, 4)));
      bool first = true;
      foreach (var s in g.Items) {
        var row = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Anchor = AnchorStyles.Left | AnchorStyles.Right, BackColor = Theme.Surface, Margin = Padding.Empty, Padding = new Padding(0, 12, 0, 12) };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 380));
        var left = Stack(Theme.Surface);
        left.Dock = DockStyle.None; left.Anchor = AnchorStyles.Left | AnchorStyles.Right;
        left.Margin = new Padding(0, 0, 24, 0);
        left.Controls.Add(Words(L.T(s), Theme.Semi(10.5f), Theme.Text, new Padding(0, 0, 0, 2)));
        string help = (L.Pt ? s.HelpPt : s.HelpEn) + (s.Restart ? L.T(" Vale na próxima vez que o jogo abrir.", " Applies the next time the game starts.") : "");
        if (help.Trim().Length > 0) left.Controls.Add(Words(help.Trim(), Theme.UI(9f), Theme.Text3, Padding.Empty));
        row.Controls.Add(left, 0, 0);

        Control editor;
        if (s.Max > s.Min) {
          var slider = new Slider { Width = 360, Anchor = AnchorStyles.Right };
          slider.Range(s.Min, s.Max, s.Step);
          slider.ValueChanged += (o, e) => Changed();
          editor = slider;
        } else if (s.Source == null && s.Choices.Count == 2 && s.Choices.Any(c => c.Value == 0 && OffWords.Contains(c.Pt)) && s.Choices.Any(c => c.Value == 1)) {
          var toggle = new ToggleSwitch { Width = 360, Anchor = AnchorStyles.Right };
          toggle.OnText = L.T(s.Choices.First(c => c.Value == 1));
          toggle.OffText = L.T(s.Choices.First(c => c.Value == 0));
          toggle.CheckedChanged += (o, e) => Changed();
          editor = toggle;
        } else {
          var box = new ChoiceBox { Width = 360, Anchor = AnchorStyles.Right };
          FillChoices(s, box, monitors, gpus);
          box.SelectedIndexChanged += (o, e) => { if (box == monitorCombo) RefillRefresh(); Changed(); };
          editor = box;
        }
        editor.Margin = Padding.Empty;
        row.Controls.Add(editor, 1, 0);
        if (!first) card.Controls.Add(new Panel { Height = 1, BackColor = Theme.Border, Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = Padding.Empty });
        first = false;
        card.Controls.Add(row);
        editors[s] = editor;
        rows[s] = row;
      }
    }

    var bottom = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = 58, ColumnCount = 3, BackColor = Theme.Bg, Padding = new Padding(0, 14, 0, 0) };
    bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
    bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
    bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
    saveState = new Label { AutoSize = true, ForeColor = Theme.Text3, Anchor = AnchorStyles.Left, Margin = new Padding(4, 10, 0, 0) };
    defaultsButton = new FlatButton(L.T("Restaurar padrões", "Restore defaults"), ButtonKind.Secondary, 190, 40) { Margin = new Padding(0, 0, 10, 0) };
    defaultsButton.Click += (s, e) => { foreach (var kv in editors) SetEditor(kv.Key, kv.Key.Default); Changed(); };
    saveButton = new FlatButton(L.T("Salvar", "Save"), ButtonKind.Primary, 150, 40) { Margin = Padding.Empty };
    saveButton.Click += (s, e) => SaveSettings();
    bottom.Controls.Add(saveState, 0, 0);
    bottom.Controls.Add(defaultsButton, 1, 0);
    bottom.Controls.Add(saveButton, 2, 0);
    pageSettings.Controls.Add(Scroller(stack));
    pageSettings.Controls.Add(bottom);
  }

  void Changed() {
    if (loading) return;
    dirty = true;
    saveState.Text = L.T("Alterações não salvas", "Unsaved changes");
    saveState.ForeColor = Theme.Text2;
  }

  void FillChoices(Setting s, ChoiceBox box, List<MonitorInfo> monitors, List<KeyValuePair<int, string>> gpus) {
    box.Items.Clear();
    switch (s.Source) {
      case "monitors":
        monitorCombo = box;
        for (int i = 0; i < Math.Max(1, monitors.Count); ++i)
          box.Items.Add(new ChoiceBox.Entry { Value = i, Text = (i + 1) + ": " + (i < monitors.Count ? monitors[i].Name : "?") });
        box.Tag = monitors;
        return;
      case "refresh":
        refreshCombo = box;
        RefillRefresh();
        return;
      case "gpus":
        box.Items.Add(new ChoiceBox.Entry { Value = -1, Text = L.T("Automática (padrão do Windows)", "Automatic (Windows default)") });
        foreach (var g in gpus) box.Items.Add(new ChoiceBox.Entry { Value = g.Key, Text = g.Value });
        return;
    }
    foreach (var c in s.Choices) box.Items.Add(new ChoiceBox.Entry { Value = c.Value, Text = L.T(c) });
  }

  void RefillRefresh() {
    if (refreshCombo == null) return;
    var monitors = monitorCombo?.Tag as List<MonitorInfo>;
    int index = monitorCombo?.Selected?.Value ?? 0;
    string device = monitors != null && index < monitors.Count ? monitors[index].Device : null;
    int keep = refreshCombo.Selected?.Value ?? 0, current = 60;
    var rates = device != null ? Displays.RefreshRates(device, out current) : new List<int>();
    refreshCombo.Items.Clear();
    refreshCombo.Items.Add(new ChoiceBox.Entry { Value = 0, Text = L.T($"A do monitor ({current} Hz)", $"Monitor refresh rate ({current} Hz)") });
    foreach (int hz in rates) refreshCombo.Items.Add(new ChoiceBox.Entry { Value = hz, Text = hz + " Hz" });
    refreshCombo.Select(keep, 0);
    refreshCombo.Invalidate();
  }

  void SetEditor(Setting s, int value) {
    switch (editors[s]) {
      case ChoiceBox box: box.Select(value, s.Default); break;
      case ToggleSwitch t: t.Checked = value != 0; break;
      case Slider sl: sl.Value = value; break;
    }
  }

  int GetEditor(Setting s) {
    switch (editors[s]) {
      case ChoiceBox box: return box.Selected?.Value ?? s.Default;
      case ToggleSwitch t: return t.Checked ? 1 : 0;
      case Slider sl: return sl.Value;
    }
    return s.Default;
  }

  bool SettingVisible(Setting s) =>
      s.Requires != "vulkan" || (root != null && cfg.VulkanDir != null && Directory.Exists(Path.Combine(root, cfg.VulkanDir)));

  void LoadSettings() {
    string ini = IniPath;
    bool ready = ini != null && Directory.Exists(Path.GetDirectoryName(ini));
    settingsNote.Text = ready
        ? L.T("As mesmas opções do menu F1 do jogo, salvas em ", "The same options as the game's F1 menu, saved in ") + Path.GetFileName(ini) +
          L.T(". O jogo usa estas opções quando abrir.", ". The game uses them when it starts.")
        : L.T("Dá para conferir as opções agora; para salvar, instale o jogo primeiro (as configurações ficam ao lado dele).",
              "You can look at the options now; to save them, install the game first (the settings are kept next to it).");
    saveButton.Enabled = ready; defaultsButton.Enabled = ready;
    loading = true;
    foreach (var kv in rows) kv.Value.Visible = SettingVisible(kv.Key);
    // Monitor first: the refresh list depends on it.
    foreach (var s in editors.Keys.OrderBy(k => k.Source == "monitors" ? 0 : 1))
      SetEditor(s, ready ? Ini.Get(ini, cfg.Section, s.Key, s.Default) : s.Default);
    loading = false;
    dirty = false;
    saveState.Text = "";
  }

  void SaveSettings() {
    string ini = IniPath;
    if (ini == null || !Directory.Exists(Path.GetDirectoryName(ini))) return;
    foreach (var s in editors.Keys) if (SettingVisible(s)) Ini.Put(ini, cfg.Section, s.Key, GetEditor(s));
    dirty = false;
    saveState.Text = L.T("Salvo. O jogo usa estas opções na próxima vez que abrir.", "Saved. The game uses these options the next time it starts.");
    saveState.ForeColor = Theme.Accent;
  }
}

// A page tab in the header: text with an accent bar under the selected one.
sealed class NavTab : Control {
  bool selected, hover;
  public NavTab(string text) {
    SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    Text = text; Font = Theme.Semi(10.5f); Cursor = Cursors.Hand;
    Size = new Size(TextRenderer.MeasureText(text, Font).Width + 36, 44);
    Margin = Padding.Empty;
  }
  public bool Selected { get => selected; set { selected = value; Invalidate(); } }
  protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
  protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Theme.Surface);
    Theme.DrawCentered(g, Text, Font, selected ? Theme.Text : hover ? Theme.Text2 : Theme.Text3, new Rectangle(0, 0, Width, Height - 4));
    if (selected) using (var b = new SolidBrush(Theme.Accent)) g.FillRectangle(b, 14, Height - 3, Width - 28, 3);
  }
}

}
