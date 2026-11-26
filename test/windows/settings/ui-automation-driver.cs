using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Automation;
using Microsoft.Win32.SafeHandles;

namespace Stone.Tests.Settings;

// A separate process using only the public UIA tree and control patterns.
// No references to StoneSettings assemblies or access to its private controls.
internal sealed class UiApplication : IDisposable
{
    private readonly Process process;
    private readonly string output;
    private readonly Task<string> stdout, stderr;
    private nint window;

    [DllImport("user32.dll")]
    private static extern bool PrintWindow(nint window, nint dc, uint flags);

    [DllImport("user32.dll")]
    private static extern nint GetForegroundWindow();

    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool WTSQuerySessionInformation(nint server, int session, int information,
        out nint buffer, out int length);

    [DllImport("wtsapi32.dll")]
    private static extern void WTSFreeMemory(nint buffer);

    internal static void RequireInteractiveDesktop()
    {
        using Process current = Process.GetCurrentProcess();
        if (!WTSQuerySessionInformation(0, current.SessionId, 25, out nint data, out int length))
        {
            Check(GetForegroundWindow() != 0,
                "UI Automation cannot access an interactive desktop. Unlock Windows and run in the interactive session.");
            return;
        }
        try
        {
            // WTSINFOEX level 1 has an aligned union at offset 8 on Windows x64.
            Check(length >= 20 && Marshal.ReadInt32(data) == 1 && Marshal.ReadInt32(data, 8) == current.SessionId,
                "Unexpected Windows session information.");
            Check(Marshal.ReadInt32(data, 16) == 1,
                "UI Automation requires an unlocked Windows session. Unlock the desktop and rerun the test.");
            Check(Marshal.ReadInt32(data, 12) == 0, "UI Automation requires an active interactive Windows session.");
        }
        finally { WTSFreeMemory(data); }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeClientProcessId(SafePipeHandle pipe, out uint processId);

    [DllImport("kernel32.dll", EntryPoint = "CreateNamedPipeW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafePipeHandle CreateNamedPipe(string name, uint openMode, uint pipeMode,
        uint instances, uint outputBuffer, uint inputBuffer, uint timeout, nint security);

    internal static NamedPipeServerStream CreatePipe(string name)
    {
        // Match the native requester's token default DACL, including restricting
        // SIDs in an isolated test session. Reject remote clients and duplicate
        // instances; WaitForConnection verifies the owned client's PID before
        // sending any data. The production client also checks owner and server PID.
        const uint duplex = 3, overlapped = 0x40000000, firstInstance = 0x00080000, rejectRemote = 8;
        SafePipeHandle handle = CreateNamedPipe(@"\\.\pipe\" + name, duplex | overlapped | firstInstance,
            rejectRemote, 1, 65536, 65536, 0, 0);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new Win32Exception(error);
        }
        try { return new NamedPipeServerStream(PipeDirection.InOut, true, false, handle); }
        catch { handle.Dispose(); throw; }
    }

    internal UiApplication(string executable, string output, params string[] arguments)
    {
        this.output = output;
        Directory.CreateDirectory(output);
        ProcessStartInfo start = new(executable)
        {
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(Path.GetDirectoryName(executable))!,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };
        foreach (string argument in arguments)
            start.ArgumentList.Add(argument);
        foreach (string directory in new[] { "temp", "local", "Logs" })
            Directory.CreateDirectory(Path.Combine(output, directory));
        start.Environment["TEMP"] = start.Environment["TMP"] = Path.Combine(output, "temp");
        start.Environment["LOCALAPPDATA"] = Path.Combine(output, "local");
        start.Environment["STONE_LOG_DIRECTORY"] = Path.Combine(output, "Logs");
        process = Process.Start(start) ?? throw new IOException("Could not start StoneSettings.");
        stdout = process.StandardOutput.ReadToEndAsync();
        stderr = process.StandardError.ReadToEndAsync();
    }

    internal AutomationElement Root
    {
        get
        {
            process.Refresh();
            if (process.HasExited)
                throw new InvalidOperationException($"StoneSettings exited early ({process.ExitCode}).");
            if (window == 0)
                window = process.MainWindowHandle;
            if (window == 0)
                throw new ElementNotAvailableException("The application window is not ready.");
            AutomationElement root = AutomationElement.FromHandle(window);
            Check(root.Current.ProcessId == process.Id, "UIA root belongs to another process.");
            return root;
        }
    }

    internal static void Check(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    internal static void Wait(string description, Func<bool> ready)
    {
        Stopwatch watch = Stopwatch.StartNew();
        Exception? last = null;
        while (watch.Elapsed < TimeSpan.FromSeconds(20))
        {
            try
            {
                if (ready())
                    return;
            }
            catch (ElementNotAvailableException error) { last = error; }
            catch (COMException error) { last = error; }
            Thread.Sleep(100);
        }
        throw new TimeoutException("Timed out waiting for " + description, last);
    }

    internal AutomationElement? Find(string id) => Root.FindFirst(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.AutomationIdProperty, id));

    internal AutomationElement Get(string id)
    {
        AutomationElement? found = null;
        Wait(id, () => (found = Find(id)) is not null);
        return found!;
    }

    internal void Invoke(string id)
    {
        Wait(id + " enabled", () => Get(id).Current.IsEnabled);
        ((InvokePattern)Get(id).GetCurrentPattern(InvokePattern.Pattern)).Invoke();
    }

    internal void Navigate(string id, string heading)
    {
        AutomationElement item = Get(id);
        if (item.TryGetCurrentPattern(SelectionItemPattern.Pattern, out object selection))
            ((SelectionItemPattern)selection).Select();
        else
            ((InvokePattern)item.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Wait("page " + heading, () => Get("SettingsPageTitle").Current.Name == heading);
    }

    internal void Search(string text)
    {
        AutomationElement search = Get("SettingsSearch");
        if (!search.TryGetCurrentPattern(ValuePattern.Pattern, out object value))
        {
            search = search.FindFirst(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Edit))
                ?? throw new InvalidOperationException("Search has no editable UIA element.");
            value = search.GetCurrentPattern(ValuePattern.Pattern);
        }
        ((ValuePattern)value).SetValue(text);
    }

    internal ToggleState ToggleState(string id) =>
        ((TogglePattern)Get(id).GetCurrentPattern(TogglePattern.Pattern)).Current.ToggleState;

    internal void Toggle(string id, bool on)
    {
        Wait(id + " enabled", () => Get(id).Current.IsEnabled);
        if (ToggleState(id) != (on ? System.Windows.Automation.ToggleState.On : System.Windows.Automation.ToggleState.Off))
            ((TogglePattern)Get(id).GetCurrentPattern(TogglePattern.Pattern)).Toggle();
        Wait(id + " state", () => ToggleState(id) == (on ? System.Windows.Automation.ToggleState.On : System.Windows.Automation.ToggleState.Off)
            && Get(id).Current.IsEnabled);
    }

    internal void SelectApp(string name)
    {
        AutomationElement combo = Get("PermissionAppSelector");
        ((ExpandCollapsePattern)combo.GetCurrentPattern(ExpandCollapsePattern.Pattern)).Expand();
        AutomationElement? choice = null;
        Wait("permission application " + name, () =>
        {
            foreach (AutomationElement item in combo.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)))
                if (item.Current.Name.Contains(name, StringComparison.Ordinal))
                    choice = item;
            return choice is not null;
        });
        ((SelectionItemPattern)choice!.GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        // Selection may collapse automatically depending on the provider.
        combo = Get("PermissionAppSelector");
        ((ExpandCollapsePattern)combo.GetCurrentPattern(ExpandCollapsePattern.Pattern)).Collapse();
    }

    internal void Close()
    {
        ((WindowPattern)Root.GetCurrentPattern(WindowPattern.Pattern)).Close();
        ExpectExit(0);
    }

    internal void ExpectExit(int code)
    {
        Check(process.WaitForExit(15000), "StoneSettings did not exit normally.");
        Check(process.ExitCode == code, $"Unexpected exit code: {process.ExitCode}, expected {code}.");
    }

    internal void WaitForConnection(NamedPipeServerStream pipe)
    {
        Task connection = pipe.WaitForConnectionAsync();
        Task exited = process.WaitForExitAsync();
        Task completed = Task.WhenAny(connection, exited, Task.Delay(TimeSpan.FromSeconds(20))).GetAwaiter().GetResult();
        if (completed != connection)
            throw new IOException(process.HasExited
                ? $"Confirmation exited before connecting ({process.ExitCode})."
                : "Confirmation did not connect within 20 seconds.");
        connection.GetAwaiter().GetResult();
        Check(GetNamedPipeClientProcessId(pipe.SafePipeHandle, out uint client) && client == process.Id,
            "Confirmation connection belongs to another process.");
    }

    internal void Capture(string name)
    {
        // UIA can publish controls before the dialog's composition animation finishes.
        Thread.Sleep(250);
        var bounds = Root.Current.BoundingRectangle;
        Check(!bounds.IsEmpty && bounds.Width > 0 && bounds.Height > 0, "Empty UIA window bounds.");
        using Bitmap bitmap = new((int)Math.Ceiling(bounds.Width), (int)Math.Ceiling(bounds.Height));
        using (Graphics graphics = Graphics.FromImage(bitmap))
        {
            nint dc = graphics.GetHdc();
            try { Check(PrintWindow(window, dc, 2), "Could not capture the owned window."); }
            finally { graphics.ReleaseHdc(dc); }
        }
        bitmap.Save(Path.Combine(output, name + ".png"), ImageFormat.Png);
    }

    internal void DumpTree(string name)
    {
        StringBuilder text = new();
        int count = 0;
        void Append(AutomationElement node, int depth)
        {
            if (++count > 500 || depth > 25)
                return;
            var current = node.Current;
            text.Append(' ', depth * 2).Append(current.ControlType.ProgrammaticName).Append(" id=")
                .Append(current.AutomationId).Append(" name=").Append(current.Name).Append(" enabled=")
                .Append(current.IsEnabled).AppendLine();
            TreeWalker walker = TreeWalker.ControlViewWalker;
            for (AutomationElement? child = walker.GetFirstChild(node); child is not null; child = walker.GetNextSibling(child))
                Append(child, depth + 1);
        }
        Append(Root, 0);
        File.WriteAllText(Path.Combine(output, name + ".uia.txt"), text.ToString());
    }

    public void Dispose()
    {
        if (!process.HasExited)
        {
            try { DumpTree("last"); Capture("last"); }
            catch (Exception error) { File.WriteAllText(Path.Combine(output, "capture-error.txt"), error.ToString()); }
            // Only the process tree created by this test is terminated on failure.
            process.Kill(true);
            process.WaitForExit(10000);
        }
        File.WriteAllText(Path.Combine(output, "stdout.txt"), stdout.GetAwaiter().GetResult());
        File.WriteAllText(Path.Combine(output, "stderr.txt"), stderr.GetAwaiter().GetResult());
        process.Dispose();
    }
}
