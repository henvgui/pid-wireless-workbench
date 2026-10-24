using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

internal sealed class DownloadProgress
{
    internal readonly object Gate = new object();
    internal string Phase = "准备下载", Detail = "正在准备固件并连接无线烧录通道…";
    internal string RecordPath = "", Error = "";
    internal int Percent, Stage = -1, ExitCode;
    internal bool Finished, Verified, Checked, HasProgress, TargetHalted;
    internal readonly Stopwatch Clock = Stopwatch.StartNew();
    internal long FinishedAt;
    private static readonly Regex ProgressLine = new Regex(@"^\[PID\] (queued|validate|probe|transfer|erase|write|verify|start|done) (\d+)%(?: bytes=(\d+)/(\d+) speed=(\d+))?\s*$");
    private static readonly string[] Codes = { "validate", "transfer", "erase", "write", "verify", "start" };
    private static readonly string[] Names = { "检查芯片与固件", "无线传输固件", "擦除目标 Flash", "写入程序", "读回校验", "启动目标程序" };

    internal void Line(string line)
    {
        lock (Gate)
        {
            Match match = ProgressLine.Match(line);
            if (match.Success)
            {
                string code = match.Groups[1].Value;
                Percent = Math.Max(0, Math.Min(99, int.Parse(match.Groups[2].Value)));
                HasProgress = true;
                Stage = Array.IndexOf(Codes, code);
                if (code == "probe") Stage = 0;
                Phase = Stage >= 0 ? Names[Stage] : code == "done" ? "确认烧录结果" : "等待任务开始";
                if (code == "done") Stage = 5;
                Detail = code == "erase" ? "正在等待目标芯片完成擦除，进度停留时仍会显示活动动画。" :
                    code == "verify" ? "正在读取目标 Flash，与本次固件逐字节核对。" :
                    code == "transfer" ? "通过主机 USB 和 ESP-NOW 发送固件，按实际任务进度更新。" :
                    code == "write" ? "正在通过从机接口写入目标芯片。" : "正在等待烧录服务确认当前步骤。";
                long done, total, rate;
                if (long.TryParse(match.Groups[3].Value, out done) && long.TryParse(match.Groups[4].Value, out total) &&
                    long.TryParse(match.Groups[5].Value, out rate) && total > 0 && done <= total)
                    Detail = "已处理 " + (done / 1024.0).ToString("F1") + " / " + (total / 1024.0).ToString("F1") +
                        " KiB · " + (rate > 0 ? (rate / 1024.0).ToString("F1") + " KiB/s" : "正在测量速度") +
                        (code == "verify" ? " · 完整读回校验" : "");
            }
            else if (line.StartsWith("[PID] Record ")) RecordPath = line.Substring(13).Trim();
            else if (line.Contains("Programming Done. Verify OK.")) { Verified = true; TargetHalted = line.Contains("Target left halted."); }
            else if (line.Contains("Check OK. No programming job submitted.")) Checked = true;
            else if (line.Contains("[PID] ERROR:") || line.Contains("Launcher ERROR:")) Error = line;
        }
    }

    internal void Complete(int exitCode)
    {
        lock (Gate)
        {
            ExitCode = exitCode;
            Finished = true;
            FinishedAt = Clock.ElapsedMilliseconds;
            if (exitCode == 0)
            {
                Percent = 100;
                Stage = 5;
                Phase = Verified ? (TargetHalted ? "烧录完成 · 目标保持暂停" : "烧录完成 · 校验通过") : Checked ? "固件预检通过" : "下载工具执行完成";
                Detail = Verified ? (TargetHalted ? "完整读回校验通过，本次没有启动目标程序。" : "任务已成功完成，目标启动结果以 Keil 日志和任务记录为准。") :
                    Checked ? "镜像检查完成，本次没有写入目标芯片。" : "详细执行结果已返回 Keil 输出区。";
            }
            else
            {
                Phase = "下载失败";
                Detail = Error.Length > 0 ? Error : "请查看 Keil 输出区或打开本次日志，未报告烧录成功。";
            }
        }
    }
}

internal sealed class LiveProgressBar : Control
{
    internal float DisplayValue;
    internal int Target;
    internal bool Waiting, Failed;
    internal float Animation;
    internal LiveProgressBar()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
        BackColor = Color.FromArgb(31, 43, 61);
    }
    internal void Tick()
    {
        DisplayValue += (Target - DisplayValue) * 0.24f;
        Animation = (Animation + 0.012f) % 1f;
        Invalidate();
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.Clear(BackColor);
        int width = Math.Max(1, Width);
        Color accent = Failed ? Color.FromArgb(255, 112, 123) : Color.FromArgb(46, 210, 197);
        if (Waiting)
        {
            int segment = Math.Max(32, width / 4);
            int x = (int)((width + segment) * Animation) - segment;
            using (Brush b = new SolidBrush(accent)) g.FillRectangle(b, x, 0, segment, Height);
        }
        else
        {
            int filled = (int)(width * DisplayValue / 100f);
            if (filled > 0)
            {
                using (Brush b = new LinearGradientBrush(new Rectangle(0, 0, width, Height),
                    Color.FromArgb(45, 139, 232), accent, 0f)) g.FillRectangle(b, 0, 0, filled, Height);
                if (!Failed && Target < 100)
                {
                    g.SetClip(new Rectangle(0, 0, filled, Height));
                    int x = (int)((width + 80) * Animation) - 80;
                    using (Brush b = new SolidBrush(Color.FromArgb(70, Color.White))) g.FillRectangle(b, x, 0, 70, Height);
                    g.ResetClip();
                }
            }
        }
    }
}

internal sealed class DownloadProgressWindow : Form
{
    private readonly DownloadProgress state;
    private readonly Label phase, detail, elapsed, percent, footer;
    private readonly Label[] stages = new Label[6];
    private readonly Button logs, hide;
    private readonly LiveProgressBar bar;
    private readonly Timer timer = new Timer();
    private readonly bool preview;
    private bool closing;
    private readonly Color accent = Color.FromArgb(46, 210, 197);

    private Label TextLabel(string value, int x, int y, int w, int h, float size, Color color)
    {
        Label label = new Label();
        label.Text = value;
        label.SetBounds(x, y, w, h);
        label.ForeColor = color;
        label.Font = new Font("Microsoft YaHei UI", size, FontStyle.Regular, GraphicsUnit.Point);
        Controls.Add(label);
        return label;
    }
    private Button ActionButton(string value, int x, int width)
    {
        Button button = new Button();
        button.Text = value;
        button.SetBounds(x, 286, width, 34);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderColor = Color.FromArgb(58, 78, 103);
        button.ForeColor = Color.FromArgb(216, 226, 239);
        button.BackColor = Color.FromArgb(25, 38, 55);
        button.Font = new Font("Microsoft YaHei UI", 9f);
        Controls.Add(button);
        return button;
    }

    internal DownloadProgressWindow(DownloadProgress progress, bool forPreview)
    {
        SuspendLayout();
        state = progress;
        preview = forPreview;
        Text = forPreview ? "无线下载 · 界面预览" : "无线下载 · PID";
        BackColor = Color.FromArgb(17, 25, 39);
        ForeColor = Color.White;
        AutoScaleDimensions = new SizeF(96, 96);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(560, 340);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        TextLabel("无线固件下载", 24, 21, 340, 32, 17f, Color.White);
        TextLabel("KEIL  →  USB  →  ESP-NOW  →  目标芯片", 25, 60, 510, 24, 9f, Color.FromArgb(145, 165, 188));
        phase = TextLabel("准备下载", 24, 108, 390, 32, 14f, Color.White);
        percent = TextLabel("…", 430, 108, 104, 32, 15f, accent);
        percent.TextAlign = ContentAlignment.TopRight;
        bar = new LiveProgressBar();
        bar.SetBounds(25, 154, 509, 12);
        Controls.Add(bar);
        string[] labels = { "检查", "传输", "擦除", "写入", "校验", "启动" };
        for (int i = 0; i < stages.Length; ++i)
        {
            stages[i] = TextLabel(labels[i], 25 + i * 86, 183, 76, 24, 9f, Color.FromArgb(104, 124, 147));
        }
        detail = TextLabel("", 24, 219, 510, 45, 9f, Color.FromArgb(171, 189, 209));
        elapsed = TextLabel("已用时 0.0 秒", 25, 291, 170, 25, 9f, Color.FromArgb(145, 165, 188));
        footer = TextLabel("完成后自动收起，结果保留在 Keil 输出区", 24, 266, 510, 20, 8f, Color.FromArgb(104, 124, 147));
        logs = ActionButton("打开日志", 330, 98);
        logs.Enabled = false;
        logs.Click += delegate { OpenLog(); };
        hide = ActionButton("收起", 440, 94);
        hide.Click += delegate { if (state.Finished) { closing = true; Close(); } else Hide(); };
        FormClosing += delegate(object sender, FormClosingEventArgs e)
        {
            lock (state.Gate)
            {
                if (!state.Finished && !closing) { e.Cancel = true; Hide(); }
            }
        };
        timer.Interval = 33;
        timer.Tick += delegate { RefreshProgress(false); };
        if (!forPreview) timer.Start();
        RefreshProgress(true);
        ResumeLayout(false);
        PerformLayout();
    }

    private void OpenLog()
    {
        string path;
        lock (state.Gate) path = state.RecordPath;
        if (path.Length == 0) return;
        string directory = Path.GetDirectoryName(path);
        if (Directory.Exists(directory)) Process.Start(new ProcessStartInfo(directory) { UseShellExecute = true });
    }

    internal void RefreshProgress(bool instant)
    {
        lock (state.Gate)
        {
            phase.Text = state.Phase;
            detail.Text = state.Detail;
            percent.Text = state.Finished && state.ExitCode != 0 ? "失败" : state.HasProgress || state.Finished ? state.Percent + "%" : "连接中";
            percent.ForeColor = state.Finished && state.ExitCode != 0 ? Color.FromArgb(255, 112, 123) : accent;
            bar.Target = state.Percent;
            bar.Waiting = (!state.HasProgress || state.Percent == 0) && !state.Finished;
            bar.Failed = state.Finished && state.ExitCode != 0;
            if (instant) bar.DisplayValue = state.Percent;
            bar.Tick();
            for (int i = 0; i < stages.Length; ++i)
                stages[i].ForeColor = i == state.Stage ? accent : i < state.Stage ? Color.FromArgb(187, 210, 228) : Color.FromArgb(104, 124, 147);
            long elapsedMs = state.Finished ? state.FinishedAt : state.Clock.ElapsedMilliseconds;
            elapsed.Text = "已用时 " + (elapsedMs / 1000.0).ToString("F1") + " 秒";
            logs.Enabled = state.RecordPath.Length > 0;
            hide.Text = state.Finished ? "关闭" : "收起";
            if (state.Finished)
            {
                footer.Text = state.ExitCode == 0 ? "结果已返回 Keil，即将自动收起" : "错误已返回 Keil，8 秒后自动收起；可打开日志查看";
                long wait = state.ExitCode == 0 ? 600 : 8000;
                if (!preview && (!Visible || state.Clock.ElapsedMilliseconds - state.FinishedAt >= wait))
                {
                    closing = true;
                    Close();
                }
            }
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) timer.Dispose();
        base.Dispose(disposing);
    }

    internal static int RenderPreview(string directory)
    {
        Directory.CreateDirectory(directory);
        StringBuilder report = new StringBuilder("{\"states\":[");
        string[] phases = { "prepare", "transfer", "write", "verify", "success", "failure" };
        foreach (string sample in phases)
        {
            DownloadProgress progress = new DownloadProgress();
            if (sample == "transfer") progress.Line("[PID] transfer 31% bytes=4096/8880 speed=8192");
            if (sample == "write") progress.Line("[PID] write 78%");
            if (sample == "verify") progress.Line("[PID] verify 94% bytes=8192/8880 speed=16384");
            if (sample == "success")
            {
                progress.Line("[PID] done 100%");
                if (progress.Percent == 100) throw new Exception("Progress cannot be complete before result validation");
                progress.Line("[PID] Programming Done. Verify OK. Application running.");
                progress.Complete(0);
            }
            if (sample == "failure")
            {
                progress.Line("[PID] write 78%");
                progress.Line("[PID] ERROR: Readback mismatch. See job.json for details.");
                progress.Complete(1);
                if (progress.Percent == 100 || progress.Verified) throw new Exception("Failure reported as successful progress");
            }
            using (DownloadProgressWindow window = new DownloadProgressWindow(progress, true))
            {
                window.StartPosition = FormStartPosition.Manual;
                window.Location = new Point(-30000, -30000);
                window.ShowInTaskbar = false;
                window.Show();
                PidWorkbench.DpiSupport.MatchInitialMonitor(window);
                Application.DoEvents();
                window.RefreshProgress(true);
                bool pmv2 = PidWorkbench.DpiSupport.AreDpiAwarenessContextsEqual(
                    PidWorkbench.DpiSupport.GetWindowDpiAwarenessContext(window.Handle), new IntPtr(-4));
                if (!pmv2) throw new Exception("Progress window is not PerMonitorV2 aware");
                foreach (Control c in window.Controls)
                    if (!window.ClientRectangle.Contains(c.Bounds)) throw new Exception("Progress control outside client area: " + c.Text);
                using (Bitmap bitmap = new Bitmap(window.Width, window.Height))
                {
                    window.DrawToBitmap(bitmap, new Rectangle(0, 0, bitmap.Width, bitmap.Height));
                    bitmap.Save(Path.Combine(directory, sample + ".png"));
                }
                if (report.Length > 12) report.Append(',');
                report.Append("{\"name\":\"").Append(sample).Append("\",\"percent\":").Append(progress.Percent)
                    .Append(",\"dpi\":").Append(window.DeviceDpi).Append(",\"pmv2\":true}");
            }
        }
        report.Append("]}");
        File.WriteAllText(Path.Combine(directory, "preview.json"), report.ToString(), Encoding.UTF8);
        return 0;
    }
}
