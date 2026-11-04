// The launcher's look: one dark palette and controls drawn by us (Windows' own buttons, combo boxes,
// sliders and tabs ignore dark colours: black text on dark, white borders).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace PortLauncher {

static class Theme {
  public static Color Bg, Surface, Surface2, Hover, Border, Text, Text2, Text3, Accent, AccentHover, OnAccent,
      Blue, BlueHover, Danger, DangerHover, Disabled, DisabledText, Link;
  public static bool Dark;

  // "system" follows Windows (Settings > Personalization > Colors > app mode), else "dark" or "light".
  public static string Mode = "system";
  static string PrefFile => System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "port-launcher", "launcher.ini");

  public static bool SystemDark() {
    try {
      using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"))
        return k?.GetValue("AppsUseLightTheme") is int v && v == 0;
    } catch (Exception) { return false; }
  }

  public static void Load() {
    int m = Ini.Get(PrefFile, "launcher", "tema", 0);
    Apply(m == 1 ? "dark" : m == 2 ? "light" : "system");
  }

  public static void Save() {
    System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(PrefFile));
    Ini.Put(PrefFile, "launcher", "tema", Mode == "dark" ? 1 : Mode == "light" ? 2 : 0);
  }

  public static void Apply(string mode) {
    Mode = mode;
    Dark = mode == "dark" || (mode == "system" && SystemDark());
    if (Dark) {
      Bg = Color.FromArgb(15, 17, 21); Surface = Color.FromArgb(24, 27, 33); Surface2 = Color.FromArgb(33, 37, 45);
      Hover = Color.FromArgb(44, 49, 59); Border = Color.FromArgb(52, 58, 69);
      Text = Color.FromArgb(240, 242, 245); Text2 = Color.FromArgb(198, 203, 212); Text3 = Color.FromArgb(150, 157, 170);
      Accent = Color.FromArgb(70, 184, 110); AccentHover = Color.FromArgb(90, 205, 130); OnAccent = Color.FromArgb(8, 26, 15);
      Blue = Color.FromArgb(64, 122, 230); BlueHover = Color.FromArgb(86, 142, 246);
      Danger = Color.FromArgb(226, 92, 84); DangerHover = Color.FromArgb(240, 114, 106);
      Disabled = Color.FromArgb(40, 44, 52); DisabledText = Color.FromArgb(118, 125, 137); Link = Color.FromArgb(118, 168, 248);
    } else {
      Bg = Color.FromArgb(241, 243, 247); Surface = Color.White; Surface2 = Color.FromArgb(240, 242, 246);
      Hover = Color.FromArgb(228, 232, 239); Border = Color.FromArgb(214, 219, 228);
      Text = Color.FromArgb(20, 23, 29); Text2 = Color.FromArgb(62, 69, 81); Text3 = Color.FromArgb(102, 110, 124);
      Accent = Color.FromArgb(28, 150, 80); AccentHover = Color.FromArgb(22, 132, 70); OnAccent = Color.White;
      Blue = Color.FromArgb(38, 99, 214); BlueHover = Color.FromArgb(30, 86, 192);
      Danger = Color.FromArgb(206, 62, 54); DangerHover = Color.FromArgb(186, 50, 44);
      Disabled = Color.FromArgb(229, 232, 237); DisabledText = Color.FromArgb(146, 152, 164); Link = Color.FromArgb(30, 96, 210);
    }
  }

  public static Font UI(float size, FontStyle style = FontStyle.Regular) => new Font("Segoe UI", size, style);
  public static Font Semi(float size) => new Font("Segoe UI Semibold", size);

  public static GraphicsPath Round(RectangleF r, float radius) {
    var p = new GraphicsPath();
    float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
    if (d <= 1) { p.AddRectangle(r); return p; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
    p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
    p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
    p.CloseFigure();
    return p;
  }

  public static void Smooth(Graphics g) {
    g.SmoothingMode = SmoothingMode.AntiAlias;
    g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
    g.PixelOffsetMode = PixelOffsetMode.HighQuality;
  }

  [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
  [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] static extern int SetWindowTheme(IntPtr hwnd, string app, string idList);

  // Dark title bar (Windows 10 2004+ / 11; older builds ignore it).
  public static void DarkTitle(Form f) {
    f.HandleCreated += (s, e) => {
      int on = Dark ? 1 : 0;
      if (DwmSetWindowAttribute(f.Handle, 20, ref on, 4) != 0) DwmSetWindowAttribute(f.Handle, 19, ref on, 4);
      int caption = ColorTranslator.ToWin32(Surface);
      DwmSetWindowAttribute(f.Handle, 35, ref caption, 4);  // caption colour (Windows 11)
    };
  }

  // Dark scroll bars on a scrolling panel or text box.
  public static void DarkScroll(Control c) {
    string theme = Dark ? "DarkMode_Explorer" : "Explorer";
    if (c.IsHandleCreated) SetWindowTheme(c.Handle, theme, null);
    c.HandleCreated += (s, e) => SetWindowTheme(c.Handle, theme, null);
  }

  public static void DrawCentered(Graphics g, string text, Font font, Color color, Rectangle r, TextFormatFlags extra = 0) =>
      TextRenderer.DrawText(g, text, font, r, color, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter |
                                                     TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding | extra);
}

// A rounded button. Stays a Button (keyboard, accessibility, AcceptButton) but draws itself.
enum ButtonKind { Primary, Blue, Secondary, Danger, Ghost }

sealed class FlatButton : Button {
  public ButtonKind Kind;
  bool hover, down;

  public FlatButton(string text, ButtonKind kind, int width, int height) {
    SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
             ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
    Text = text; Kind = kind; Size = new Size(width, height);
    Font = Theme.Semi(10.5f); Cursor = Cursors.Hand; FlatStyle = FlatStyle.Flat; BackColor = Color.Transparent;
    FlatAppearance.BorderSize = 0;
  }

  protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
  protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
  protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
  protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
  protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }

  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Parent?.BackColor ?? Theme.Bg);
    Theme.Smooth(g);
    Color fill, text = Color.White, border = Color.Empty;
    switch (Kind) {
      case ButtonKind.Primary: fill = hover ? Theme.AccentHover : Theme.Accent; text = Theme.OnAccent; break;
      case ButtonKind.Blue: fill = hover ? Theme.BlueHover : Theme.Blue; break;
      case ButtonKind.Danger: fill = hover ? Theme.DangerHover : Theme.Danger; break;
      case ButtonKind.Ghost: fill = hover ? Theme.Surface2 : (Parent?.BackColor ?? Theme.Bg); text = Theme.Text2; break;
      default: fill = hover ? Theme.Hover : Theme.Surface2; text = Theme.Text; border = Theme.Border; break;
    }
    if (!Enabled) { fill = Theme.Disabled; text = Theme.DisabledText; border = Color.Empty; }
    if (down && Enabled) fill = ControlPaint.Dark(fill, 0.08f);
    var r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
    using (var path = Theme.Round(r, 7)) {
      using (var b = new SolidBrush(fill)) g.FillPath(b, path);
      if (border != Color.Empty) using (var p = new Pen(border)) g.DrawPath(p, path);
      if (Focused && ShowFocusCues && Enabled) using (var p = new Pen(Theme.Text, 1.5f)) g.DrawPath(p, path);
    }
    Theme.DrawCentered(g, Text, Font, text, ClientRectangle);
  }
}

// A value picker: shows the current choice, opens a dark list on click; left/right arrows step.
sealed class ChoiceBox : Control {
  public sealed class Entry { public int Value; public string Text; }
  public readonly List<Entry> Items = new List<Entry>();
  int index = -1;
  bool hover;
  public event EventHandler SelectedIndexChanged;

  public ChoiceBox() {
    SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
             ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
    Size = new Size(360, 36); Cursor = Cursors.Hand; Font = Theme.UI(10f); TabStop = true;
  }

  public int SelectedIndex {
    get => index;
    set { int v = Items.Count == 0 ? -1 : Math.Max(0, Math.Min(Items.Count - 1, value)); if (v != index) { index = v; Invalidate(); SelectedIndexChanged?.Invoke(this, EventArgs.Empty); } }
  }
  public Entry Selected => index >= 0 && index < Items.Count ? Items[index] : null;

  public void Select(int value, int fallback) {
    int pick = Items.FindIndex(i => i.Value == value);
    if (pick < 0) pick = Items.FindIndex(i => i.Value == fallback);
    SelectedIndex = pick >= 0 ? pick : 0;
  }

  protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
  protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
  protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
  protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
  protected override bool IsInputKey(Keys k) => k == Keys.Left || k == Keys.Right || base.IsInputKey(k);

  protected override void OnKeyDown(KeyEventArgs e) {
    if (e.KeyCode == Keys.Left && index > 0) SelectedIndex = index - 1;
    else if (e.KeyCode == Keys.Right && index < Items.Count - 1) SelectedIndex = index + 1;
    else if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) Open();
    base.OnKeyDown(e);
  }

  protected override void OnMouseDown(MouseEventArgs e) {
    Focus();
    if (Enabled && e.Button == MouseButtons.Left) Open();
    base.OnMouseDown(e);
  }

  void Open() {
    var menu = new ContextMenuStrip { Renderer = new DarkMenuRenderer(), ShowImageMargin = false, ShowCheckMargin = true,
                                      Font = Font, BackColor = Theme.Surface2, ForeColor = Theme.Text };
    for (int i = 0; i < Items.Count; ++i) {
      int pick = i;
      var item = new ToolStripMenuItem(Items[i].Text) { Checked = i == index, ForeColor = Theme.Text, Padding = new Padding(4, 6, 4, 6) };
      item.Click += (s, e) => SelectedIndex = pick;
      menu.Items.Add(item);
    }
    menu.MinimumSize = new Size(Width, 0);
    menu.Closed += (s, e) => BeginInvoke(new Action(menu.Dispose));
    menu.Show(this, new Point(0, Height + 2));
  }

  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Parent?.BackColor ?? Theme.Surface);
    Theme.Smooth(g);
    var r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
    using (var path = Theme.Round(r, 7)) {
      using (var b = new SolidBrush(!Enabled ? Theme.Disabled : hover ? Theme.Hover : Theme.Surface2)) g.FillPath(b, path);
      using (var p = new Pen(Focused ? Theme.Accent : Theme.Border, Focused ? 1.5f : 1f)) g.DrawPath(p, path);
    }
    var textRect = new Rectangle(12, 0, Width - 44, Height);
    TextRenderer.DrawText(g, Selected?.Text ?? "", Font, textRect, Enabled ? Theme.Text : Theme.DisabledText,
                          TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
    // chevron
    float cx = Width - 20, cy = Height / 2f;
    using (var p = new Pen(Enabled ? Theme.Text2 : Theme.DisabledText, 1.8f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
      g.DrawLines(p, new[] { new PointF(cx - 5, cy - 2.5f), new PointF(cx, cy + 2.5f), new PointF(cx + 5, cy - 2.5f) });
  }
}

// An on/off switch with its state written next to it.
sealed class ToggleSwitch : Control {
  bool on;
  public string OnText = "On", OffText = "Off";
  public event EventHandler CheckedChanged;

  public ToggleSwitch() {
    SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
             ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
    Size = new Size(200, 36); Cursor = Cursors.Hand; Font = Theme.UI(10f); TabStop = true;
  }

  public bool Checked { get => on; set { if (value != on) { on = value; Invalidate(); CheckedChanged?.Invoke(this, EventArgs.Empty); } } }

  protected override void OnMouseDown(MouseEventArgs e) { Focus(); if (Enabled && e.Button == MouseButtons.Left) Checked = !on; base.OnMouseDown(e); }
  protected override void OnKeyDown(KeyEventArgs e) { if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) Checked = !on; base.OnKeyDown(e); }
  protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
  protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Parent?.BackColor ?? Theme.Surface);
    Theme.Smooth(g);
    var track = new RectangleF(1, (Height - 24) / 2f, 44, 24);
    using (var path = Theme.Round(track, 12)) {
      using (var b = new SolidBrush(!Enabled ? Theme.Disabled : on ? Theme.Accent : Theme.Border)) g.FillPath(b, path);
      if (Focused) using (var p = new Pen(Theme.Text, 1.5f)) g.DrawPath(p, path);
    }
    float knob = on ? track.Right - 21 : track.X + 3;
    using (var b = new SolidBrush(Enabled ? Color.White : Theme.DisabledText)) g.FillEllipse(b, knob, track.Y + 3, 18, 18);
    TextRenderer.DrawText(g, on ? OnText : OffText, Font, new Rectangle(56, 0, Width - 56, Height),
                          Enabled ? Theme.Text : Theme.DisabledText, TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
  }
}

// A slider with a filled track and the value at its end.
sealed class Slider : Control {
  int value, min, max = 100, step = 5;
  bool drag;
  public string Suffix = "%";
  public event EventHandler ValueChanged;

  public Slider() {
    SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
             ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
    Size = new Size(360, 36); Cursor = Cursors.Hand; Font = Theme.Semi(10f); TabStop = true;
  }

  public void Range(int minimum, int maximum, int stepSize) { min = minimum; max = maximum; step = Math.Max(1, stepSize); }
  public int Value {
    get => value;
    set { int v = Math.Max(min, Math.Min(max, value)); if (v != this.value) { this.value = v; Invalidate(); ValueChanged?.Invoke(this, EventArgs.Empty); } }
  }

  RectangleF Track => new RectangleF(10, Height / 2f - 3, Width - 80, 6);

  void SetFromX(int x) {
    var t = Track;
    float f = Math.Max(0, Math.Min(1, (x - t.X) / t.Width));
    Value = (int)Math.Round((min + f * (max - min)) / step) * step;
  }

  protected override void OnMouseDown(MouseEventArgs e) { Focus(); if (Enabled) { drag = true; SetFromX(e.X); } base.OnMouseDown(e); }
  protected override void OnMouseMove(MouseEventArgs e) { if (drag) SetFromX(e.X); base.OnMouseMove(e); }
  protected override void OnMouseUp(MouseEventArgs e) { drag = false; base.OnMouseUp(e); }
  protected override bool IsInputKey(Keys k) => k == Keys.Left || k == Keys.Right || base.IsInputKey(k);
  protected override void OnKeyDown(KeyEventArgs e) {
    if (e.KeyCode == Keys.Left) Value -= step;
    if (e.KeyCode == Keys.Right) Value += step;
    base.OnKeyDown(e);
  }
  protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
  protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Parent?.BackColor ?? Theme.Surface);
    Theme.Smooth(g);
    var t = Track;
    float f = max > min ? (value - min) / (float)(max - min) : 0;
    using (var path = Theme.Round(t, 3)) using (var b = new SolidBrush(Theme.Border)) g.FillPath(b, path);
    var fill = new RectangleF(t.X, t.Y, Math.Max(6, t.Width * f), t.Height);
    using (var path = Theme.Round(fill, 3)) using (var b = new SolidBrush(Enabled ? Theme.Accent : Theme.DisabledText)) g.FillPath(b, path);
    float kx = t.X + t.Width * f;
    using (var b = new SolidBrush(Enabled ? Color.White : Theme.DisabledText)) g.FillEllipse(b, kx - 9, Height / 2f - 9, 18, 18);
    if (Focused) using (var p = new Pen(Theme.Accent, 2f)) g.DrawEllipse(p, kx - 10, Height / 2f - 10, 20, 20);
    TextRenderer.DrawText(g, value + Suffix, Font, new Rectangle(Width - 60, 0, 60, Height), Enabled ? Theme.Text : Theme.DisabledText,
                          TextFormatFlags.VerticalCenter | TextFormatFlags.Right | TextFormatFlags.NoPadding);
  }
}

// A thin progress bar in the accent colour.
sealed class ProgressLine : Control {
  int value, maximum = 1;
  public ProgressLine() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true); Height = 6; }
  public int Maximum { get => maximum; set { maximum = Math.Max(1, value); Invalidate(); } }
  public int Value { get => value; set { this.value = Math.Max(0, Math.Min(maximum, value)); Invalidate(); } }
  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Parent?.BackColor ?? Theme.Bg);
    Theme.Smooth(g);
    var r = new RectangleF(0, 0, Width - 1, Height - 1);
    using (var path = Theme.Round(r, Height / 2f)) using (var b = new SolidBrush(Theme.Surface2)) g.FillPath(b, path);
    if (value > 0) {
      var f = new RectangleF(0, 0, Math.Max(Height, (Width - 1) * value / (float)maximum), Height - 1);
      using (var path = Theme.Round(f, Height / 2f)) using (var b = new SolidBrush(Theme.Accent)) g.FillPath(b, path);
    }
  }
}

// A rounded card that holds other controls.
sealed class Card : Panel {
  public Card() {
    SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    BackColor = Theme.Surface; Padding = new Padding(22, 18, 22, 20);
  }
  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Parent?.BackColor ?? Theme.Bg);
    Theme.Smooth(g);
    using (var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 10)) {
      using (var b = new SolidBrush(BackColor)) g.FillPath(b, path);
      using (var p = new Pen(Theme.Border)) g.DrawPath(p, path);
    }
  }
}

// A status mark: a filled circle with a check, a cross or a spinner dot.
sealed class StatusDot : Control {
  public bool? Ok;
  public StatusDot() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor, true); Size = new Size(26, 26); }
  protected override void OnPaint(PaintEventArgs e) {
    var g = e.Graphics;
    g.Clear(Parent?.BackColor ?? Theme.Surface);
    Theme.Smooth(g);
    var color = Ok == null ? Theme.Text3 : Ok == true ? Theme.Accent : Theme.Danger;
    using (var b = new SolidBrush(Color.FromArgb(48, color))) g.FillEllipse(b, 1, 1, Width - 3, Height - 3);
    using (var p = new Pen(color, 2.2f) { StartCap = LineCap.Round, EndCap = LineCap.Round }) {
      float c = Width / 2f - 0.5f;
      if (Ok == true) g.DrawLines(p, new[] { new PointF(c - 5, c), new PointF(c - 1.5f, c + 3.5f), new PointF(c + 5.5f, c - 4) });
      else if (Ok == false) { g.DrawLine(p, c - 4, c - 4, c + 4, c + 4); g.DrawLine(p, c + 4, c - 4, c - 4, c + 4); }
      else using (var b = new SolidBrush(color)) g.FillEllipse(b, c - 3, c - 3, 6, 6);
    }
  }
}

sealed class DarkMenuRenderer : ToolStripProfessionalRenderer {
  sealed class Colors : ProfessionalColorTable {
    public override Color ToolStripDropDownBackground => Theme.Surface2;
    public override Color MenuBorder => Theme.Border;
    public override Color MenuItemSelected => Theme.Hover;
    public override Color MenuItemBorder => Theme.Hover;
    public override Color ImageMarginGradientBegin => Theme.Surface2;
    public override Color ImageMarginGradientMiddle => Theme.Surface2;
    public override Color ImageMarginGradientEnd => Theme.Surface2;
    public override Color CheckBackground => Theme.Surface2;
    public override Color CheckSelectedBackground => Theme.Hover;
    public override Color CheckPressedBackground => Theme.Hover;
  }
  public DarkMenuRenderer() : base(new Colors()) { RoundedEdges = false; }
  protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e) { e.TextColor = Theme.Text; base.OnRenderItemText(e); }
  protected override void OnRenderItemCheck(ToolStripItemImageRenderEventArgs e) {
    var g = e.Graphics;
    Theme.Smooth(g);
    var r = e.ImageRectangle;
    using (var p = new Pen(Theme.Accent, 2f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
      g.DrawLines(p, new[] { new PointF(r.X + 3, r.Y + r.Height / 2f), new PointF(r.X + r.Width / 2f - 1, r.Bottom - 4), new PointF(r.Right - 3, r.Y + 4) });
  }
}

// Message boxes in the launcher's colours.
sealed class Dialog : Form {
  Dialog(string title, string text, string yes, string no, bool danger) {
    Text = title; FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = MinimizeBox = false; ShowInTaskbar = false;
    StartPosition = FormStartPosition.CenterParent; BackColor = Theme.Surface; ForeColor = Theme.Text;
    Font = Theme.UI(10f); AutoScaleMode = AutoScaleMode.Dpi; Padding = new Padding(24, 20, 24, 18);
    Theme.DarkTitle(this);
    var body = new Label { Text = text, AutoSize = true, MaximumSize = new Size(560, 0), ForeColor = Theme.Text2, Dock = DockStyle.Top };
    var bar = new FlowLayoutPanel { Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft, Height = 52, Padding = new Padding(0, 14, 0, 0) };
    var ok = new FlatButton(yes, danger ? ButtonKind.Danger : ButtonKind.Primary, 130, 38) { DialogResult = DialogResult.Yes };
    bar.Controls.Add(ok);
    if (no != null) {
      var cancel = new FlatButton(no, ButtonKind.Secondary, 130, 38) { DialogResult = DialogResult.No, Margin = new Padding(0, 3, 10, 3) };
      bar.Controls.Add(cancel);
      CancelButton = cancel;
    } else CancelButton = ok;
    AcceptButton = ok;
    Controls.Add(body);
    Controls.Add(bar);
    Load += (s, e) => {
      ClientSize = new Size(Math.Max(420, body.PreferredSize.Width + Padding.Horizontal), body.PreferredSize.Height + 52 + Padding.Vertical + 12);
    };
  }

  public static bool Ask(IWin32Window owner, string title, string text, string yes, string no, bool danger = false) {
    using (var d = new Dialog(title, text, yes, no, danger)) return d.ShowDialog(owner) == DialogResult.Yes;
  }
  public static void Info(IWin32Window owner, string title, string text) {
    using (var d = new Dialog(title, text, "OK", null, false)) d.ShowDialog(owner);
  }
}

}
