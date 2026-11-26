

namespace StoneSettings.Models;

public sealed class AppCatalog
{
    public int Format
    {
        get; set;
    }
    public bool PackageManagementAvailable
    {
        get; set;
    }
    public List<InstalledApp> Apps { get; set; } = [];
}
