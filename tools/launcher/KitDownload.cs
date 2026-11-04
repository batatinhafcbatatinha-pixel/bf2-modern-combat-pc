// Standalone installer: the launcher downloaded on its own fetches the port's source kit (the
// release ZIP of the game's repository: scripts and source code, nothing from the game) and
// unpacks it into the chosen folder, then copies itself there.
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Reflection;

namespace PortLauncher {

static class KitDownload {
  public static string Fetch(GameConfig cfg, string target, Action<string> log) {
    if (string.IsNullOrEmpty(cfg.Repo))
      throw new InvalidOperationException("launcher.json has no repo.");
    target = Path.GetFullPath(target);
    if (Directory.Exists(target) && Directory.EnumerateFileSystemEntries(target).Any() &&
        !File.Exists(Path.Combine(target, "setup.json")))
      throw new IOException(L.T("A pasta escolhida não está vazia: ", "The chosen folder is not empty: ") + target);
    // The newest release (pre-releases too: releases/latest skips them): its source-kit ZIP, or for
    // repositories without one, GitHub's source ZIP of that release's tag.
    string tag = NewestTag(cfg.Repo);
    string url = string.IsNullOrEmpty(cfg.KitAsset)
        ? $"https://github.com/{cfg.Repo}/archive/refs/tags/{tag}.zip"
        : $"https://github.com/{cfg.Repo}/releases/download/{tag}/{cfg.KitAsset}";
    string zip = Path.Combine(Path.GetTempPath(), cfg.Repo.Replace('/', '-') + "-kit.zip");
    log(L.T("Baixando ", "Downloading ") + url);
    using (var web = new WebClient()) {
      web.Headers[HttpRequestHeader.UserAgent] = "port-launcher";
      web.DownloadFile(url, zip);
    }
    log(L.T("Extraindo para ", "Extracting to ") + target);
    Directory.CreateDirectory(target);
    string full = target.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
    using (var archive = ZipFile.OpenRead(zip)) {
      // The ZIP holds one top folder (<repo>/...): its contents go straight into the target.
      string prefix = archive.Entries.Select(e => e.FullName.Split('/')[0]).Distinct().Count() == 1
          ? archive.Entries[0].FullName.Split('/')[0] + "/" : "";
      foreach (var entry in archive.Entries) {
        string rel = entry.FullName.Substring(prefix.Length);
        if (rel.Length == 0) continue;
        string dest = Path.GetFullPath(Path.Combine(target, rel.Replace('/', Path.DirectorySeparatorChar)));
        if (!dest.StartsWith(full, StringComparison.OrdinalIgnoreCase)) continue;  // no paths outside the folder
        if (rel.EndsWith("/")) { Directory.CreateDirectory(dest); continue; }
        Directory.CreateDirectory(Path.GetDirectoryName(dest));
        entry.ExtractToFile(dest, true);
      }
    }
    File.Delete(zip);
    string self = Assembly.GetExecutingAssembly().Location;
    string copy = Path.Combine(target, Path.GetFileName(self));
    if (!string.Equals(Path.GetFullPath(self), copy, StringComparison.OrdinalIgnoreCase)) File.Copy(self, copy, true);
    log("OK");
    return target;
  }

  static string NewestTag(string repo) {
    using (var web = new WebClient()) {
      web.Headers[HttpRequestHeader.UserAgent] = "port-launcher";
      web.Headers[HttpRequestHeader.Accept] = "application/vnd.github+json";
      string json = web.DownloadString($"https://api.github.com/repos/{repo}/releases?per_page=1");
      var list = new System.Web.Script.Serialization.JavaScriptSerializer().DeserializeObject(json) as object[];
      if (list == null || list.Length == 0 || !(list[0] is System.Collections.Generic.IDictionary<string, object> release))
        throw new IOException("No release found for " + repo);
      return Convert.ToString(release["tag_name"]);
    }
  }
}

}
