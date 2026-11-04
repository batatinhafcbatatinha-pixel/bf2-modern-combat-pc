// The game's launcher.json: what to install, where the game and its settings file are, and the
// settings shown on the PC settings tab (the same keys the in-game F1 menu reads and writes).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Web.Script.Serialization;

namespace PortLauncher {

sealed class Choice {
  public int Value;
  public string Pt, En;
}

sealed class Setting {
  public string Key, Pt, En, HelpPt, HelpEn;
  public int Default;
  public string Source;         // null, "monitors", "refresh", "gpus"
  public List<Choice> Choices = new List<Choice>();
  public int Min, Max, Step;    // slider when Max > Min
  public bool Restart;          // read when the game starts (shown for information only)
  public string Requires;       // e.g. "vulkan": hidden when that part is missing
}

sealed class Group {
  public string Pt, En;
  public List<Setting> Items = new List<Setting>();
}

sealed class ToolDep {
  public string Id, Name, Check;
  public double Gb;
}

sealed class GameConfig {
  public double BuildToolsGb = 7, PythonGb = 0.1;
  public List<ToolDep> Tools = new List<ToolDep>();
  public Dictionary<string, string> GameFiles = new Dictionary<string, string>();

  public string Game, Repo, KitAsset, Exe, Ini, Section = "opcoes", Play, Setup = "tools/setup.ps1";
  public string Disc, Minutes, VulkanDir;
  public int DiskGb;
  public List<Group> Groups = new List<Group>();

  static string Str(IDictionary<string, object> d, string k, string def = null) =>
      d.TryGetValue(k, out var v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : def;
  static int Int(IDictionary<string, object> d, string k, int def = 0) =>
      d.TryGetValue(k, out var v) && v != null ? Convert.ToInt32(v, CultureInfo.InvariantCulture) : def;
  static IEnumerable<IDictionary<string, object>> List(IDictionary<string, object> d, string k) =>
      d.TryGetValue(k, out var v) && v is IEnumerable e ? e.OfType<IDictionary<string, object>>() : Enumerable.Empty<IDictionary<string, object>>();

  public static GameConfig Parse(string json) {
    var d = (IDictionary<string, object>)new JavaScriptSerializer().DeserializeObject(json);
    var c = new GameConfig {
      Game = Str(d, "game", "Game"), Repo = Str(d, "repo"), KitAsset = Str(d, "kit_asset"), Exe = Str(d, "exe"),
      Ini = Str(d, "ini"), Section = Str(d, "section", "opcoes"), Play = Str(d, "play"),
      Setup = Str(d, "setup", "tools/setup.ps1"), Disc = Str(d, "disc", ""), Minutes = Str(d, "minutes", ""),
      DiskGb = Int(d, "disk_gb"), VulkanDir = Str(d, "vulkan_dir"),
    };
    if (d.TryGetValue("deps", out var depsObj) && depsObj is IDictionary<string, object> deps) {
      if (deps.ContainsKey("buildtools_gb")) c.BuildToolsGb = Convert.ToDouble(deps["buildtools_gb"], CultureInfo.InvariantCulture);
      if (deps.ContainsKey("python_gb")) c.PythonGb = Convert.ToDouble(deps["python_gb"], CultureInfo.InvariantCulture);
      foreach (var t in List(deps, "tools"))
        c.Tools.Add(new ToolDep { Id = Str(t, "id"), Name = Str(t, "name"), Check = Str(t, "check"),
                                  Gb = t.TryGetValue("gb", out var gb) ? Convert.ToDouble(gb, CultureInfo.InvariantCulture) : 0 });
      if (deps.TryGetValue("game_files", out var gf) && gf is IDictionary<string, object> files)
        foreach (var kv in files) c.GameFiles[kv.Key] = Convert.ToString(kv.Value);
    }
    foreach (var g in List(d, "groups")) {
      var group = new Group { Pt = Str(g, "pt"), En = Str(g, "en") };
      foreach (var s in List(g, "items")) {
        var item = new Setting {
          Key = Str(s, "key"), Pt = Str(s, "pt"), En = Str(s, "en"), HelpPt = Str(s, "help_pt", ""),
          HelpEn = Str(s, "help_en", ""), Default = Int(s, "default"), Source = Str(s, "source"),
          Min = Int(s, "min"), Max = Int(s, "max"), Step = Math.Max(1, Int(s, "step", 1)),
          Restart = s.TryGetValue("restart", out var r) && r is bool b && b, Requires = Str(s, "requires"),
        };
        foreach (var ch in List(s, "choices"))
          item.Choices.Add(new Choice { Value = Int(ch, "v"), Pt = Str(ch, "pt"), En = Str(ch, "en", Str(ch, "pt")) });
        group.Items.Add(item);
      }
      c.Groups.Add(group);
    }
    return c;
  }

  // launcher.json next to the launcher (a kit folder) wins over the copy built into the exe.
  public static GameConfig Load(string dir) {
    string path = Path.Combine(dir, "launcher.json");
    if (File.Exists(path)) return Parse(File.ReadAllText(path));
    using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("launcher.json")) {
      if (s == null) return null;
      using (var r = new StreamReader(s)) return Parse(r.ReadToEnd());
    }
  }
}

static class L {
  public static readonly bool Pt = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "pt";
  public static string T(string pt, string en) => Pt ? pt : en;
  public static string T(Setting s) => Pt ? s.Pt : s.En;
  public static string T(Choice c) => Pt ? c.Pt : c.En;
  public static string T(Group g) => Pt ? g.Pt : g.En;
}

}
