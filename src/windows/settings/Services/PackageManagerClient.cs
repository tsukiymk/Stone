using System.Diagnostics;
using System.Text.Json;
using StoneSettings.Helpers;
using StoneSettings.Models;

namespace StoneSettings.Services;

internal sealed class PackageManagerClient(string executable)
{
    public async Task<PackageOperationResult> ExecuteAsync(string operation, string? target, string dataRoot, string? image,
        string? confirmationPipe = null, Action<int>? started = null, CancellationToken cancellation = default)
    {
        if (!File.Exists(executable))
            throw new FileNotFoundException(UiText.Get("RuntimeMissing"), executable);
        ProcessStartInfo start = new(executable)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,
            StandardErrorEncoding = System.Text.Encoding.UTF8
        };
        foreach (string value in new[] { "--manage-package", operation, "--data-dir", dataRoot })
            start.ArgumentList.Add(value);
        if (target is not null)
        {
            start.ArgumentList.Add("--target");
            start.ArgumentList.Add(target);
        }
        if (!string.IsNullOrEmpty(image))
        {
            start.ArgumentList.Add("--system-image");
            start.ArgumentList.Add(image);
        }
        if (confirmationPipe is not null)
        {
            if (operation != "install")
                throw new ArgumentException("Only installation reuses the settings window.");
            start.ArgumentList.Add("--confirmation-host-pipe");
            start.ArgumentList.Add(confirmationPipe);
            start.ArgumentList.Add("--confirmation-host-parent");
            start.ArgumentList.Add(Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
        cancellation.ThrowIfCancellationRequested();
        using Process process = Process.Start(start) ?? throw new IOException(UiText.Get("StartStoneFailed"));
        started?.Invoke(process.Id);
        // The install window cancels only before sending an approval. Once the
        // transaction starts it disables cancellation and waits for its result.
        using var registration = cancellation.Register(() =>
        {
            try
            {
                if (!process.HasExited)
                    process.Kill();
            }
            catch (InvalidOperationException)
            {
            }
            catch (System.ComponentModel.Win32Exception)
            {
            }
        });
        string? resultLine = null;
        async Task ReadOutput()
        {
            while (await process.StandardOutput.ReadLineAsync().ConfigureAwait(false) is string line)
                if (line.StartsWith("STONE_PACKAGE_RESULT ", StringComparison.Ordinal)
                    && System.Text.Encoding.UTF8.GetByteCount(line) <= 65536)
                    resultLine = line["STONE_PACKAGE_RESULT ".Length..];
        }
        async Task DrainErrors()
        {
            char[] buffer = new char[4096];
            while (await process.StandardError.ReadAsync(buffer).ConfigureAwait(false) != 0)
            {
            }
        }
        await Task.WhenAll(ReadOutput(), DrainErrors(), process.WaitForExitAsync()).ConfigureAwait(false);
        cancellation.ThrowIfCancellationRequested();
        PackageOperationResult? result = resultLine is null ? null
            : JsonSerializer.Deserialize(resultLine, SettingsJson.Default.PackageOperationResult);
        if (result?.Format != 1 || result.Compatibility is { IsValid: false }
            || result.Compatibility is not null && result.Status != "failed"
            || !(result.Status == "success" && process.ExitCode == 0
                || result.Status == "cancelled" && process.ExitCode == 3 || result.Status == "failed" && process.ExitCode == 1))
            throw new IOException(UiText.Get("PrepareAndroidFailed"));
        return result;
    }
}
