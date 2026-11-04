using System;
using System.IO;
using System.Net;
using System.Windows.Forms;

namespace PortLauncher {

static class Program {
  [STAThread]
  static void Main(string[] args) {
    ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
    Application.EnableVisualStyles();
    Application.SetCompatibleTextRenderingDefault(false);
    string dir = AppDomain.CurrentDomain.BaseDirectory;
    var config = GameConfig.Load(dir);
    if (config == null) {
      MessageBox.Show(L.T("Este launcher não tem a configuração do jogo (launcher.json).",
                          "This launcher has no game configuration (launcher.json)."), "Launcher",
                      MessageBoxButtons.OK, MessageBoxIcon.Error);
      return;
    }
    Theme.Load();
    // Test aid: --shot <file.png> [tab index] draws the window into a PNG and exits (no clicks needed);
    // --theme dark|light overrides the saved theme for that run.
    int themeArg = Array.IndexOf(args, "--theme");
    if (themeArg >= 0 && themeArg + 1 < args.Length) Theme.Apply(args[themeArg + 1]);
    string kit = MainForm.IsKit(dir) ? dir : null;
    int page = 0;
    while (true) {
      var form = new MainForm(config, kit, page);
      int shot = Array.IndexOf(args, "--shot");
      if (shot >= 0 && shot + 1 < args.Length)
        form.ShotOnLoad(args[shot + 1], shot + 2 < args.Length && int.TryParse(args[shot + 2], out int tab) ? tab : 0);
      Application.Run(form);
      if (!form.Restart) break;
      kit = form.RootForRestart;   // a kit downloaded in this session stays
      page = form.PageForRestart;
    }
  }
}

}
