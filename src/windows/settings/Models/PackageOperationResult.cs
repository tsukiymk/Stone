

namespace StoneSettings.Models;

internal sealed class PackageOperationResult
{
    public int Format
    {
        get; set;
    }
    public string Status { get; set; } = "";
    public string PackageName { get; set; } = "";
    public string Message { get; set; } = "";
    public PackageCompatibilityFailure? Compatibility { get; set; }
}

internal sealed class PackageCompatibilityFailure
{
    public string Reason { get; set; } = "";
    public string Required { get; set; } = "";
    public string Available { get; set; } = "";

    internal bool IsValid => Reason is { Length: > 0 and <= 64 }
        && Required is { Length: <= 4096 } && Available is { Length: <= 4096 };
}
