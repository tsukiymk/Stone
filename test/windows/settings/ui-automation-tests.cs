using System;
using System.Buffers.Binary;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows.Automation;
using static Stone.Tests.Settings.UiApplication;

namespace Stone.Tests.Settings;

internal static class UiAutomationTests
{
    private const string Package = "com.trebledot.stone.uia";
    private const string OtherPackage = "com.trebledot.stone.uia.undeclared";
    private static string executable = "", output = "";

    [MTAThread]
    private static int Main(string[] args)
    {
        try
        {
            Check(args.Length is 2 or 3, "Usage: ui-automation-tests <Debug-settings.exe> <Debug-output-directory> [protocol-worker.exe]");
            executable = Path.GetFullPath(args[0]);
            string debug = Path.GetDirectoryName(Path.GetDirectoryName(executable))!;
            Check(Path.GetFileName(debug) == "Debug" && Path.GetFileName(Path.GetDirectoryName(debug)) == "x64"
                && File.Exists(executable), "Build and use bin/x64/Debug/settings/StoneSettings.exe.");
            string root = Path.GetFullPath(args[1]);
            Check(root.StartsWith(debug + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase),
                "UIA outputs and profiles must stay under bin/x64/Debug.");
            output = Path.Combine(root, Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(output);
            Console.WriteLine("Evidence: " + output);
            RequireInteractiveDesktop();
            // UIA calls run on this MTA thread. The target owns a different UI thread/process.
            foreach (var (language, title, button) in new[] {
                ("en-US", "Welcome to Stone", "Choose system image"),
                ("zh-CN", "欢迎使用 Stone", "选择系统镜像"),
                ("ja-JP", "Stone へようこそ", "システムイメージを選択") })
                Run("welcome-" + language, directory => Welcome(directory, language, title, button));
            Run("cached-settings", CachedSettings);
            if (args.Length == 3)
            {
                string source = Path.GetDirectoryName(executable)!;
                string runtime = Path.Combine(output, "package-runtime");
                string settings = Path.Combine(runtime, "settings");
                foreach (string file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
                {
                    string target = Path.Combine(settings, Path.GetRelativePath(source, file));
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    File.Copy(file, target);
                }
                string worker = Path.GetFullPath(args[2]);
                File.Copy(worker, Path.Combine(runtime, "StoneClient.exe"));
                foreach (string suffix in new[] { ".dll", ".deps.json", ".runtimeconfig.json" })
                    File.Copy(Path.ChangeExtension(worker, suffix), Path.Combine(runtime, Path.GetFileName(Path.ChangeExtension(worker, suffix))));
                string normal = executable;
                executable = Path.Combine(settings, "StoneSettings.exe");
                try
                {
                    Run("open-installed-app", OpenInstalledApp);
                }
                finally { executable = normal; }
            }
            Run("install-approve", directory => Confirm(directory, "install", "approve"));
            Run("install-cancel", directory => Confirm(directory, "install", "cancel"));
            Run("install-close", directory => Confirm(directory, "install", "close"));
            Run("update-approve", directory => Confirm(directory, "install", "approve", updating: true));
            Run("update-cancel", directory => Confirm(directory, "install", "cancel", updating: true));
            Run("update-close", directory => Confirm(directory, "install", "close", updating: true));
            Run("unverified-install", directory => Confirm(directory, "install", "cancel", verified: false));
            Run("notifications-deny", directory => Confirm(directory, "notifications", "cancel"));
            Run("notifications-allow", directory => Confirm(directory, "notifications", "approve"));
            Run("notifications-close", directory => Confirm(directory, "notifications", "close"));
            File.WriteAllText(Path.Combine(output, "passed.txt"), "PASS: external UI Automation scenarios completed.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            if (Directory.Exists(output))
                File.WriteAllText(Path.Combine(output, "failure.txt"), error.ToString());
            return 1;
        }
    }

    private static void Run(string name, Action<string> test)
    {
        string directory = Path.Combine(output, name);
        Directory.CreateDirectory(directory);
        test(directory);
        Console.WriteLine("PASS: " + name);
    }

    private static void Welcome(string directory, string language, string title, string button)
    {
        using UiApplication app = new(executable, directory, "--data-dir", Path.Combine(directory, "profile"),
            "--system-image", Path.Combine(directory, "absent.img"), "--language", language);
        Wait("localized welcome heading", () => app.Get("WelcomeTitle").Current.Name == title);
        AutomationElement choose = app.Get("WelcomeChooseImage");
        Check(choose.Current.Name == button && choose.Current.IsEnabled, "Localized image selection is unavailable.");
        Check(choose.TryGetCurrentPattern(InvokePattern.Pattern, out _), "Image selection has no Invoke pattern.");
        app.DumpTree("welcome");
        app.Capture("welcome");
        app.Close();
        Check(!File.Exists(Path.Combine(directory, "profile", "system-image.path")), "Welcome persisted an unselected image.");
    }

    private static void CachedSettings(string directory)
    {
        string profile = Path.Combine(directory, "profile");
        string catalog = Path.Combine(profile, "data", "system", "stone-packages", "packages.json");
        Directory.CreateDirectory(Path.GetDirectoryName(catalog)!);
        File.WriteAllText(catalog, JsonSerializer.Serialize(new {
            format = 1, packageManagementAvailable = false, apps = new[] {
                new { packageName = Package, name = "Alpha UIA", version = "1.0", uid = 10000,
                    storageIdentity = "10000:1:2", requestedPermissions = new[] {
                        "android.permission.POST_NOTIFICATIONS", "android.permission.MANAGE_EXTERNAL_STORAGE" } },
                new { packageName = OtherPackage, name = "Beta UIA", version = "1.0", uid = 10001,
                    storageIdentity = "10001:3:4", requestedPermissions = Array.Empty<string>() }
            }
        }));
        string runtime = Path.Combine(profile, "runtime-features.properties");
        const string originalRuntime = "format=1\naot=true\njit=true\nexecutor=art\n";
        File.WriteAllText(runtime, originalRuntime);
        string image = Path.Combine(directory, "invalid.img");
        File.WriteAllText(image, "Deliberately invalid image for UIA failure recovery.");
        using UiApplication app = new(executable, directory, "--data-dir", profile, "--system-image", image, "--language", "en-US");
        // Exercise the normal failed validation path; cached metadata is not a working backend.
        AutomationElement error = app.Get("SettingsErrorDialog");
        Check(error.Current.Name.Contains("Unable to load", StringComparison.Ordinal), "Expected image validation failure was not shown.");
        AutomationElement close = error.FindFirst(TreeScope.Descendants, new AndCondition(
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button),
            new PropertyCondition(AutomationElement.NameProperty, "Close")))
            ?? throw new InvalidOperationException("The real error dialog has no close action.");
        app.Capture("validation-error");
        ((InvokePattern)close.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Wait("validation failure recovered", () => app.Find("SettingsErrorDialog") is null);
        app.Navigate("AppsNavigation", "Apps");
        Wait("cached applications", () => ListCount(app) == 2);
        Check(!app.Get("InstallApk").Current.IsEnabled, "An unavailable backend enabled installation.");
        app.Search("__uia_no_matching_app__");
        Wait("empty filtered list", () => ListCount(app) == 0);
        app.Search(OtherPackage);
        Wait("package-name filtering", () => ListCount(app) == 1 && app.Find(OtherPackage) is not null);
        app.Search("Alpha UIA");
        Wait("display-name filtering", () => ListCount(app) == 1 && app.Find(Package) is not null);
        app.Search("");
        Wait("cleared filter", () => ListCount(app) == 2);
        AutomationElement first = app.Get("AppsList").FindFirst(TreeScope.Children,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem));
        ((InvokePattern)first.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Check(app.Get("OpenApp").Current.Name == "Open app" && !app.Get("OpenApp").Current.IsEnabled,
            "Application details lost its launch action or enabled it without a working backend.");
        app.DumpTree("application-details");
        app.Capture("application-details");
        AutomationElement done = app.Root.FindFirst(TreeScope.Descendants, new AndCondition(
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button),
            new PropertyCondition(AutomationElement.NameProperty, "Done")))!;
        ((InvokePattern)done.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Wait("application details closed", () => app.Find("OpenApp") is null);
        app.Navigate("SystemNavigation", "System");
        Check(app.Get("SystemImagePath").Current.Name == image, "Selected image path was lost.");
        Check(!File.Exists(Path.Combine(profile, "system-image.path")), "An invalid image was remembered.");
        app.Navigate("NotificationsNavigation", "Notifications");
        string notifications = Path.Combine(profile, "data", "system", "stone-notifications", "policy.properties");
        app.Toggle("AllNotifications", false);
        Wait("global notification denial persisted", () => HasLine(notifications, "global=false"));
        app.Toggle("AllNotifications", true);
        Wait("global notification restoration persisted", () => HasLine(notifications, "global=true"));
        app.Navigate("PermissionsNavigation", "Permissions");
        app.SelectApp("Alpha UIA");
        Wait("declared controls", () => app.Get("PermissionAllFiles").Current.IsEnabled && app.Get("PermissionNotifications").Current.IsEnabled);
        string storage = Path.Combine(profile, "data", "system", "stone-storage-access", Package + ".properties");
        app.Toggle("PermissionAllFiles", true);
        Wait("storage grant persisted with identity", () => HasLine(storage, "allowed=true") && HasLine(storage, "identity=10000:1:2"));
        app.Toggle("PermissionNotifications", true);
        Wait("per-app notification grant persisted", () => HasLine(notifications, "package." + Package + "=true"));
        app.DumpTree("permissions");
        app.Capture("permissions");
        app.Toggle("PermissionAllFiles", false);
        app.Toggle("PermissionNotifications", false);
        Wait("permission restoration persisted", () => HasLine(storage, "allowed=false") && HasLine(notifications, "package." + Package + "=false"));
        app.SelectApp("Beta UIA");
        Wait("undeclared permissions disabled", () => !app.Get("PermissionAllFiles").Current.IsEnabled && !app.Get("PermissionNotifications").Current.IsEnabled);
        Check(!File.Exists(Path.Combine(Path.GetDirectoryName(storage)!, OtherPackage + ".properties"))
            && !File.ReadAllText(notifications).Contains("package." + OtherPackage + "=", StringComparison.Ordinal),
            "Another application's permission policy changed.");
        app.Capture("undeclared");
        app.Close();
        Check(File.ReadAllText(runtime) == originalRuntime, "Settings rewrote native runtime configuration.");
    }

    private static int ListCount(UiApplication app) => app.Get("AppsList").FindAll(TreeScope.Children,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)).Count;

    private static bool HasLine(string file, string line)
    {
        try { return File.Exists(file) && File.ReadAllLines(file).Contains(line, StringComparer.Ordinal); }
        catch (IOException) { return false; } // Policy files are replaced atomically by the target.
    }

    private static UiApplication PackagePresentation(string directory, string language)
    {
        string profile = Path.Combine(directory, "profile");
        string catalog = Path.Combine(profile, "data", "system", "stone-packages", "packages.json");
        Directory.CreateDirectory(Path.GetDirectoryName(catalog)!);
        File.WriteAllText(catalog, JsonSerializer.Serialize(new { format = 1, packageManagementAvailable = true,
            apps = new[] { new { packageName = Package, name = "Stone UIA fixture", version = "1.0", uid = 10001,
                installer = "com.trebledot.stone.settings", codeBytes = 65536 } } }));
        string image = Path.Combine(directory, "presentation.img");
        File.WriteAllText(image, "Protocol presentation fixture; not an Android system image.");
        UiApplication app = new(executable, directory, "--data-dir", profile, "--system-image", image, "--language", language);
        Wait("presentation backend ready", () => app.Get("InstallApk").Current.IsEnabled && ListCount(app) == 1);
        return app;
    }

    private static void OpenInstalledApp(string directory)
    {
        using UiApplication app = PackagePresentation(directory, "zh-CN");
        AutomationElement first = app.Get("AppsList").FindFirst(TreeScope.Children,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem));
        ((InvokePattern)first.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Check(app.Get("OpenApp").Current.Name == "打开应用" && app.Get("OpenApp").Current.IsEnabled, "Launch action unavailable");
        app.Capture("application-details");
        app.DumpTree("application-details");
        app.Invoke("OpenApp");
        string marker = Path.Combine(directory, "profile", "launch-arguments.json");
        Wait("installed package launch request", () => File.Exists(marker));
        string[] sent = JsonSerializer.Deserialize<string[]>(File.ReadAllText(marker))!;
        Check(sent.SequenceEqual(new[] { "--package", Package, "--data-dir", Path.Combine(directory, "profile"),
            "--system-image", Path.Combine(directory, "presentation.img") }), "Open app changed package/data/image");
        Wait("details dismissed after launch", () => app.Find("OpenApp") is null);
        app.Close();
    }

    private static void Confirm(string directory, string operation, string action, bool updating = false, bool verified = true)
    {
        Guid id = Guid.NewGuid();
        string name = $"Stone.Settings.{Environment.ProcessId}.{id:D}";
        using NamedPipeServerStream pipe = CreatePipe(name);
        using UiApplication app = new(executable, directory, "--confirmation-pipe", name,
            "--confirmation-parent", Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "--data-dir", Path.Combine(directory, "profile"), "--language", "en-US");
        app.WaitForConnection(pipe);
        Wait("confirmation window", () => { _ = app.Root; return true; });
        byte[] payload = JsonSerializer.SerializeToUtf8Bytes(new {
            format = 1, parentProcessId = Environment.ProcessId, requestId = id.ToString("D"),
            confirmation = new { operation, packageName = Package, name = "Stone UIA fixture", version = "2.0",
                previousVersion = updating ? "1.0" : "", installer = "Stone UIA tests", verified,
                requestedPermissions = new[] { "android.permission.CAMERA", "com.trebledot.stone.uia.CUSTOM" } }
        });
        byte[] length = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(length, (uint)payload.Length);
        pipe.Write(length);
        pipe.Write(payload);
        AutomationElement primary = app.Get("ConfirmPrimary");
        Check(primary.Current.IsEnabled == verified, "Unverified installation enabled approval.");
        if (operation == "notifications")
        {
            Check(primary.Current.Name == "Allow" && app.Get("ConfirmCancel").Current.Name == "Don't allow",
                "Notification decision labels are incorrect.");
            Wait("default denial focus", () => app.Get("ConfirmCancel").Current.HasKeyboardFocus);
            AutomationElementCollection windows = AutomationElement.RootElement.FindAll(TreeScope.Children,
                new AndCondition(new PropertyCondition(AutomationElement.ProcessIdProperty, app.Root.Current.ProcessId),
                    new PropertyCondition(AutomationElement.IsOffscreenProperty, false)));
            Check(windows.Count == 1, "Notification permission must show only the dialog without an overlay.");
        }
        app.DumpTree("confirmation");
        app.Capture("confirmation");
        if (action == "close")
        {
            // Closing must never produce approval, including an EOF during cancellation.
            ((WindowPattern)app.Root.GetCurrentPattern(WindowPattern.Pattern)).Close();
            byte[] decision = new byte[1];
            int read = pipe.ReadAsync(decision).AsTask().WaitAsync(TimeSpan.FromSeconds(15)).GetAwaiter().GetResult();
            Check(read == 0 || decision[0] == 0, "Closing approved a package request.");
            if (read != 0)
                pipe.WriteByte(0xac);
        }
        else
        {
            app.Invoke(action == "approve" ? "ConfirmPrimary" : "ConfirmCancel");
            byte[] decision = new byte[1];
            pipe.ReadExactlyAsync(decision).AsTask().WaitAsync(TimeSpan.FromSeconds(15)).GetAwaiter().GetResult();
            Check(decision[0] == (action == "approve" ? 1 : 0), "UI decision disagrees with the confirmation protocol.");
            pipe.WriteByte(0xac);
        }
        app.ExpectExit(0);
    }
}
