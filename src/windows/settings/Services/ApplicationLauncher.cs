using System.Diagnostics;
using System.Text.RegularExpressions;
using StoneSettings.Helpers;

namespace StoneSettings.Services;

internal static partial class ApplicationLauncher
{
    [GeneratedRegex(@"\A[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+\z", RegexOptions.CultureInvariant)]
    private static partial Regex PackageNamePattern();

    internal static ProcessStartInfo CreateStartInfo(string executable, string packageName, string dataRoot, string? image)
    {
        if (packageName.Length > 255 || !PackageNamePattern().IsMatch(packageName))
            throw new InvalidDataException(UiText.Get("InvalidAppPackage"));
        ProcessStartInfo start = new(executable) { UseShellExecute = false, CreateNoWindow = true };
        foreach (string value in new[] { "--package", packageName, "--data-dir", dataRoot })
            start.ArgumentList.Add(value);
        if (!string.IsNullOrEmpty(image))
        {
            start.ArgumentList.Add("--system-image");
            start.ArgumentList.Add(image);
        }
        return start;
    }

    internal static void Open(string executable, string packageName, string dataRoot, string? image)
    {
        if (!File.Exists(executable)) throw new FileNotFoundException(UiText.Get("RuntimeMissing"), executable);
        using Process process = Process.Start(CreateStartInfo(executable, packageName, dataRoot, image))
            ?? throw new IOException(UiText.Get("StartStoneFailed"));
    }
}
