

namespace StoneSettings.Models;

internal sealed record ApplicationSnapshot(bool Loaded, bool CanManagePackages, List<InstalledApp> Apps, List<InstalledApp> NotificationApps);
