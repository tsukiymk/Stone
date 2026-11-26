using System.Text.Json;
using StoneSettings.Models;

namespace StoneSettings.Services;

internal sealed class ApplicationCatalogService
{
    internal async Task<ApplicationSnapshot> LoadAsync(string dataRoot, bool backendAvailable)
    {
        string path = Path.Combine(dataRoot, "data", "system", "stone-packages", "packages.json");
        AppCatalog? catalog = await Task.Run(async () =>
        {
            if (!File.Exists(path))
                return null;
            await using FileStream input = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete,
                4096, FileOptions.Asynchronous | FileOptions.SequentialScan);
            if (input.Length > 8 * 1024 * 1024)
                throw new InvalidDataException("Application catalog exceeds its size limit.");
            return await JsonSerializer.DeserializeAsync(input, SettingsJson.Default.AppCatalog);
        });
        if (catalog is not null && (catalog.Format != 1 || catalog.Apps.Any(app => string.IsNullOrWhiteSpace(app.PackageName))))
            throw new InvalidDataException("Invalid application catalog.");
        List<InstalledApp> apps = catalog?.Apps.OrderBy(app => app.Name, StringComparer.CurrentCultureIgnoreCase).ToList() ?? [];
        List<InstalledApp> observed = await Task.Run(async () =>
        {
            List<InstalledApp> result = [];
            string directory = Path.Combine(dataRoot, "data", "system", "stone-notifications", "apps");
            if (!Directory.Exists(directory))
                return result;
            int count = 0;
            foreach (string identity in Directory.EnumerateFiles(directory, "*.json"))
            {
                if (++count > 10000)
                    throw new InvalidDataException("Too many notification application identities.");
                await using FileStream input = new(identity, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete,
                    4096, FileOptions.Asynchronous | FileOptions.SequentialScan);
                if (input.Length > 65536)
                    throw new InvalidDataException("Notification application identity is too large.");
                AppCatalog? entry = await JsonSerializer.DeserializeAsync(input, SettingsJson.Default.AppCatalog);
                if (entry?.Format != 1 || entry.Apps.Count != 1)
                    throw new InvalidDataException("Invalid notification application identity.");
                InstalledApp app = entry.Apps[0];
                if (!NotificationPolicy.IsPackageName(app.PackageName) || Path.GetFileNameWithoutExtension(identity) != app.PackageName
                        || app.Uid is < 10000 or > 19999 || string.IsNullOrWhiteSpace(app.Name))
                    throw new InvalidDataException("Invalid notification application identity.");
                result.Add(app);
            }
            return result;
        });
        List<InstalledApp> notificationApps = apps.Concat(observed).GroupBy(app => app.PackageName, StringComparer.Ordinal)
            .Select(group => group.First()).OrderBy(app => app.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        foreach (InstalledApp app in notificationApps)
            app.IconPng = await ApplicationIconData.LoadAsync(dataRoot, app.PackageName);
        return new(catalog is not null, backendAvailable && catalog?.PackageManagementAvailable == true, apps, notificationApps);
    }
}
