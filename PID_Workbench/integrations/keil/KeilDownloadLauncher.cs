// A Windows-subsystem entry point avoids opening a blank Windows Terminal tab.
// stdout/stderr still go to the inherited Keil output pipe; child exit codes
// are preserved. Build with /target:winexe, never /target:exe.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8")]

internal static class KeilDownloadLauncher
{
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetStdHandle(int kind);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(IntPtr file, byte[] data, uint size,
                                         out uint written, IntPtr overlapped);
    private static readonly object OutputLock = new object();
    private static DownloadProgress progress;

    private static void Output(string line)
    {
        if (line == null) return;
        if (progress != null) progress.Line(line);
        lock (OutputLock)
        {
            IntPtr output = GetStdHandle(-11);
            if (output == IntPtr.Zero || output == new IntPtr(-1)) output = GetStdHandle(-12);
            if (output == IntPtr.Zero || output == new IntPtr(-1)) return;
            byte[] data = Encoding.Default.GetBytes(line + "\r\n");
            uint written;
            WriteFile(output, data, (uint)data.Length, out written, IntPtr.Zero);
        }
    }

    // Windows CRT quoting, including embedded quotes and trailing backslashes.
    private static string Quote(string argument)
    {
        StringBuilder value = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in argument)
        {
            if (c == '\\') { ++slashes; continue; }
            if (c == '"') value.Append('\\', slashes * 2 + 1);
            else value.Append('\\', slashes);
            value.Append(c);
            slashes = 0;
        }
        value.Append('\\', slashes * 2);
        return value.Append('"').ToString();
    }

    private static int RunChild(string[] args)
    {
        try
        {
            if (args.Length < 2 || !File.Exists(args[0]))
                throw new ArgumentException("Expected Python executable followed by adapter arguments.");
            StringBuilder arguments = new StringBuilder();
            for (int i = 1; i < args.Length; ++i)
            {
                if (i > 1) arguments.Append(' ');
                arguments.Append(Quote(args[i]));
            }
            ProcessStartInfo info = new ProcessStartInfo(args[0], arguments.ToString());
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardInput = true;
            info.RedirectStandardOutput = true;
            info.RedirectStandardError = true;
            info.StandardOutputEncoding = Encoding.UTF8;
            info.StandardErrorEncoding = Encoding.UTF8;
            info.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            info.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            using (Process child = new Process())
            {
                child.StartInfo = info;
                child.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) { Output(e.Data); };
                child.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) { Output(e.Data); };
                Output("[PID] Wireless download started; real-time progress is shown in the download window.");
                child.Start();
                child.StandardInput.Close();
                child.BeginOutputReadLine();
                child.BeginErrorReadLine();
                child.WaitForExit(); // Waits for both async readers to reach EOF.
                Output("[PID] Download tool finished (exit " + child.ExitCode + ").");
                return child.ExitCode;
            }
        }
        catch (Exception error)
        {
            Output("[PID] Launcher ERROR: " + error.Message);
            return 1;
        }
    }

    private static string[] RetryArguments(string[] original, string recordPath)
    {
        if (original.Length < 2 || !File.Exists(original[1]) || !File.Exists(recordPath))
            return (string[])original.Clone(); // Conversion/launch failed before a record was saved.
        List<string> result = new List<string>(new string[] { original[0], original[1], "--retry-record", recordPath });
        for (int i = 2; i + 1 < original.Length; ++i)
            if (original[i] == "--timeout") { result.Add(original[i]); result.Add(original[i + 1]); break; }
        return result.ToArray();
    }

    private static void StartWorker(string[] args)
    {
        Thread worker = new Thread(delegate()
        {
            int exitCode = RunChild(args);
            progress.Complete(exitCode);
        });
        worker.IsBackground = true;
        worker.Start();
    }

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "--render-preview")
        {
            Application.EnableVisualStyles();
            try { return DownloadProgressWindow.RenderPreview(args[1]); }
            catch (Exception error) { Output("[PID] Preview ERROR: " + error.Message); return 1; }
        }
        if (Environment.GetEnvironmentVariable("PID_KEIL_NO_PROGRESS") == "1") return RunChild(args);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        progress = new DownloadProgress();
        using (DownloadProgressWindow window = new DownloadProgressWindow(progress, false))
        {
            if (Environment.GetEnvironmentVariable("PID_KEIL_TEST_OFFSCREEN") == "1")
            {
                window.StartPosition = FormStartPosition.Manual;
                window.Location = new System.Drawing.Point(-30000, -30000);
                window.ShowInTaskbar = false;
            }
            window.RetryRequested += delegate(string recordPath)
            {
                try { StartWorker(RetryArguments(args, recordPath)); }
                catch (Exception error) { Output("[PID] Launcher ERROR: " + error.Message); progress.Complete(1); }
            };
            window.Shown += delegate { PidWorkbench.DpiSupport.MatchInitialMonitor(window); StartWorker(args); };
            Application.Run(window);
            return progress.ExitCode;
        }
    }
}
