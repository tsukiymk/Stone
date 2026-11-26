using System.Text;
using System.Text.Json;
using StoneSettings.Helpers;
using StoneSettings.Models;

namespace StoneSettings.Services;

internal static class StorageAccessPolicy
{
    private static string PathFor(string root, string package) => Path.Combine(root, "data", "system", "stone-storage-access", package + ".properties");
    public static bool Load(string root, InstalledApp app)
    {
        if (!NotificationPolicy.IsPackageName(app.PackageName))
            throw new InvalidDataException("Invalid storage permission package.");
        string path = PathFor(root, app.PackageName);
        if (!app.RequestsAllFiles || string.IsNullOrEmpty(app.StorageIdentity) || !File.Exists(path))
            return false;
        using FileStream input = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (input.Length > 65536)
            throw new InvalidDataException("Storage permission policy exceeds its limit.");
        using StreamReader reader = new(input, Encoding.UTF8, true);
        string? identity = null;
        bool allowed = false;
        while (reader.ReadLine() is string raw)
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#') || line.StartsWith('!'))
                continue;
            int split = line.IndexOf('=');
            if (split <= 0)
                throw new InvalidDataException("Invalid storage permission policy.");
            string key = line[..split], value = line[(split + 1)..];
            if (key == "identity")
                identity = value.Replace("\\:", ":", StringComparison.Ordinal);
            else if (key == "allowed")
                allowed = value switch
                {
                    "true" => true,
                    "false" => false,
                    _ => throw new InvalidDataException("Invalid storage permission state.")
                };
            else
                throw new InvalidDataException("Invalid storage permission key.");
        }
        return identity == app.StorageIdentity && allowed;
    }
    public static Task SetAsync(string root, InstalledApp app, bool allowed) => Task.Run(() =>
    {
        if (!NotificationPolicy.IsPackageName(app.PackageName) || string.IsNullOrEmpty(app.StorageIdentity)
                || app.StorageIdentity.Any(value => !(char.IsAsciiDigit(value) || value is ':' or '-'))
                || allowed && !app.RequestsAllFiles)
            throw new InvalidDataException("Application cannot receive all-files access.");
        string path = PathFor(root, app.PackageName);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using FileStream gate = new(Path.ChangeExtension(path, ".lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        DateTime deadline = DateTime.UtcNow.AddSeconds(5);
        while (true)
        {
            try
            {
                gate.Lock(0, 1);
                break;
            }
            catch (IOException error) when ((error.HResult & 0xffff) == 33 && DateTime.UtcNow < deadline)
            {
                Thread.Sleep(20);
            }
        }
        try
        {
            string catalog = Path.Combine(root, "data", "system", "stone-packages", "packages.json");
            using FileStream input = new(catalog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (input.Length > 8 * 1024 * 1024)
                throw new InvalidDataException("Application catalog exceeds its limit.");
            AppCatalog state = JsonSerializer.Deserialize(input, SettingsJson.Default.AppCatalog) ?? throw new InvalidDataException("Missing application catalog.");
            InstalledApp? current = state.Apps.SingleOrDefault(value => value.PackageName == app.PackageName);
            if (current is null)
            {
                string observedPath = Path.Combine(root, "data", "system", "stone-notifications", "apps", app.PackageName + ".json");
                using FileStream observed = new(observedPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                if (observed.Length > 65536)
                    throw new InvalidDataException("Application identity exceeds its limit.");
                AppCatalog identity = JsonSerializer.Deserialize(observed, SettingsJson.Default.AppCatalog) ?? throw new InvalidDataException("Missing application identity.");
                if (identity.Format != 1 || identity.Apps.Count != 1 || identity.Apps[0].PackageName != app.PackageName)
                    throw new InvalidDataException("Invalid application identity.");
                current = identity.Apps[0];
            }
            if (state.Format != 1 || current.Uid != app.Uid || current.StorageIdentity != app.StorageIdentity || allowed && !current.RequestsAllFiles)
                throw new InvalidDataException(UiText.Get("AppIdentityChanged"));
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (FileStream output = new(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    output.Write(Encoding.UTF8.GetBytes($"identity={app.StorageIdentity}\nallowed={(allowed ? "true" : "false")}\n"));
                    output.Flush(true);
                }
                File.Move(temporary, path, true);
            }
            finally
            {
                if (File.Exists(temporary))
                    File.Delete(temporary);
            }
        }
        finally
        {
            gate.Unlock(0, 1);
        }
    });
}
