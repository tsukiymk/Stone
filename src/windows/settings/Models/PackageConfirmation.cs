

namespace StoneSettings.Models;

internal sealed class PackageConfirmationData
{
    public string Operation { get; set; } = "";
    public string PackageName { get; set; } = "";
    public string Name { get; set; } = "";
    public string Version { get; set; } = "";
    public string PreviousVersion { get; set; } = "";
    public string Installer { get; set; } = "";
    public string IconPng { get; set; } = "";
    public string[]? RequestedPermissions { get; set; }
    public bool Verified
    {
        get; set;
    }
}
