using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.Text.Json.Serialization;

[assembly: WinRT.GeneratedWinRTExposedExternalType(typeof(ObservableCollection<StoneSettings.Models.InstalledApp>))]
[assembly: WinRT.GeneratedWinRTExposedExternalType(typeof(string[]))]

namespace StoneSettings.Models;

[WinRT.GeneratedBindableCustomProperty]
[WinRT.GeneratedWinRTExposedType]
public sealed partial class InstalledApp : INotifyPropertyChanged
{
    public string PackageName { get; set; } = "";
    public string Name { get; set; } = "";
    public string Version { get; set; } = "";
    public int Uid
    {
        get; set;
    }
    public string Installer { get; set; } = "";
    public long CodeBytes
    {
        get; set;
    }
    public string[] RequestedPermissions { get; set; } = [];
    public string StorageIdentity { get; set; } = "";
    [JsonIgnore] public string IconPng { get; set; } = "";
    [JsonIgnore] public bool RequestsNotifications => RequestedPermissions.Contains("android.permission.POST_NOTIFICATIONS", StringComparer.Ordinal);
    [JsonIgnore] public bool RequestsAllFiles => RequestedPermissions.Contains("android.permission.MANAGE_EXTERNAL_STORAGE", StringComparer.Ordinal);
    private bool notificationsEnabled;
    [JsonIgnore]
    public bool NotificationsEnabled
    {
        get => notificationsEnabled;
        set
        {
            if (notificationsEnabled == value)
                return;
            notificationsEnabled = value;
            PropertyChanged?.Invoke(this, new(nameof(NotificationsEnabled)));
        }
    }
    public event PropertyChangedEventHandler? PropertyChanged;
    // Selector item automation peers use the item's string representation.
    public override string ToString() => Name;
    [JsonIgnore] public string Initial => StringInfo.GetNextTextElement(string.IsNullOrWhiteSpace(Name) ? "?" : Name).ToUpperInvariant();
    public bool Matches(string query) => Name.Contains(query, StringComparison.CurrentCultureIgnoreCase)
        || PackageName.Contains(query, StringComparison.OrdinalIgnoreCase);
}
