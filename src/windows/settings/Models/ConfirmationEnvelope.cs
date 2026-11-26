

namespace StoneSettings.Models;

internal sealed class ConfirmationEnvelope
{
    public int Format
    {
        get; set;
    }
    public string RequestId { get; set; } = "";
    public uint ParentProcessId
    {
        get; set;
    }
    public PackageConfirmationData Confirmation { get; set; } = new();
}
