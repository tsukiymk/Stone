using System.Text.Json;
using System.Text.Json.Serialization;
using System.Xml.Linq;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;
using StoneSettings.ViewModels;

// Build-only WinRT marker and resource adapter: the tests execute the actual
// view model, result decoder and launch argument builder without loading XAML.
namespace WinRT { internal sealed class GeneratedWinRTExposedTypeAttribute : Attribute { } }
namespace StoneSettings.Helpers
{
    internal static class UiText
    {
        private static Dictionary<string, string> strings = new();
        internal static void Load(string root, string language) => strings = XDocument.Load(Path.Combine(root, language, "Resources.resw"))
            .Root!.Elements("data").ToDictionary(value => (string)value.Attribute("name")!, value => (string)value.Element("value")!);
        internal static string Get(string key) => strings[key];
        internal static string Format(string key, params object[] args) => string.Format(Get(key), args);
    }
}
namespace StoneSettings.Models
{
    [JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
    [JsonSerializable(typeof(PackageOperationResult))]
    internal partial class SettingsJson : JsonSerializerContext { }
}
internal static class PackageUiTests
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    private static async Task<int> Main(string[] args)
    {
        // Protocol-only worker for the external UIA suite. Never installs or executes an APK.
        if (args.FirstOrDefault() == "--package")
        {
            string root = args[Array.IndexOf(args, "--data-dir") + 1];
            File.WriteAllText(Path.Combine(root, "launch-arguments.json"), JsonSerializer.Serialize(args));
            return 0;
        }
        if (args.FirstOrDefault() == "--manage-package")
        {
            if (args[1] == "list")
            {
                Console.WriteLine("STONE_PACKAGE_RESULT {\"format\":1,\"status\":\"success\",\"message\":\"\"}");
                return 0;
            }
            string variant = Path.GetFileNameWithoutExtension(args[Array.IndexOf(args, "--target") + 1]);
            PackageOperationResult result = new() { Format = 1, Status = "failed", Message = "native diagnostic",
                Compatibility = new() { Reason = "native-abi", Required = "arm64-v8a, armeabi", Available = "x86_64" } };
            if (variant == "oversized") result.Compatibility.Required = new string('a', 4097);
            if (variant == "oversized-frame") result.Message = new string('界', 30000);
            if (variant == "unknown") result.Compatibility.Reason = "future-check";
            if (variant == "legacy") result.Compatibility = null;
            Console.WriteLine("STONE_PACKAGE_RESULT " + JsonSerializer.Serialize(result, SettingsJson.Default.PackageOperationResult));
            return 1;
        }
        foreach (var (language, heading, title) in new[] {
            ("zh-CN", "无法安装此应用", "应用架构不兼容"), ("en-US", "Cannot install this app", "App architecture is incompatible"),
            ("ja-JP", "このアプリはインストールできません", "アプリのアーキテクチャが非対応です") })
        {
            UiText.Load(args[0], language);
            PackageConfirmationData request = new() { Operation = "install", Name = "测试 app.apk", Verified = true };
            PackageOperationResult failure = new() { Message = "original diagnostic", Compatibility = new() {
                Reason = "native-abi", Required = "arm64-v8a, armeabi", Available = "x86_64" } };
            PackageConfirmationPageViewModel model = new(request, null, failure);
            Check(model.IsFailure && !model.CanApprove && model.Heading == heading && model.FailureTitle == title, "Failure state/translation");
            Check(model.FailureReason.Contains("arm64-v8a, armeabi") && model.FailureReason.Contains("x86_64")
                && model.FailureSuggestion.Contains("x86_64") && model.FailureDetails == failure.Message, "ABI explanation and guidance");
            foreach (string kind in new[] { "android-api", "image-abi", "shared-uid", "split-apk" })
            {
                failure.Compatibility = new() { Reason = kind, Required = "38", Available = "37" };
                model = new(request, null, failure);
                Check(model.IsFailure && !model.CanApprove && model.FailureSuggestion.Length > 0, "Missing failure guidance: " + kind);
                if (kind == "android-api") Check(model.FailureReason.Contains("38") && model.FailureReason.Contains("37"), "SDK values lost");
            }
            failure.Compatibility = new() { Reason = "future-check" };
            model = new(request, null, failure);
            Check(model.FailureReason == failure.Message && !model.CanApprove, "Unknown failures lost their actual diagnostic");
            Check(new PackageConfirmationPageViewModel(request, "reading").CanApprove == false, "Preparation enabled installation");
            Check(new PackageConfirmationPageViewModel(request, null).CanApprove, "Verified normal confirmation regressed");
        }
        var start = ApplicationLauncher.CreateStartInfo("StoneClient.exe", "me.him188.ani", "D:\\test data\\应用", "D:\\image path\\system.img");
        Check(!start.UseShellExecute && start.ArgumentList.SequenceEqual(new[] { "--package", "me.him188.ani", "--data-dir",
            "D:\\test data\\应用", "--system-image", "D:\\image path\\system.img" }), "Launch changed identity or split path arguments");
        Check(ApplicationLauncher.CreateStartInfo("StoneClient.exe", "com.example.app", "data", null).ArgumentList.Count == 4, "Optional image");
        foreach (string name in new[] { "--apk", "com.example app", "../app", "com.example\napp", "" })
        {
            try { ApplicationLauncher.CreateStartInfo("StoneClient.exe", name, "data", null); throw new Exception("Invalid package accepted"); }
            catch (InvalidDataException) { }
        }
        PackageManagerClient client = new(Path.ChangeExtension(typeof(PackageUiTests).Assembly.Location, ".exe"));
        foreach (string variant in new[] { "abi", "unknown", "legacy" })
        {
            var result = await client.ExecuteAsync("install", variant, args[1], null);
            Check(result.Status == "failed" && (variant == "legacy" ? result.Compatibility is null : result.Compatibility?.Required == "arm64-v8a, armeabi"), "Structured result decoding");
        }
        foreach (string variant in new[] { "oversized", "oversized-frame" })
        {
            try { await client.ExecuteAsync("install", variant, args[1], null); throw new Exception("Oversized failure accepted"); }
            catch (IOException) { }
        }
        Console.WriteLine("PASS localized installation failure states, structured result transport and installed-app launch arguments.");
        return 0;
    }
}
