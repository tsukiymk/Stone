using System.Collections.ObjectModel;
using StoneSettings.Helpers;
using StoneSettings.Models;

namespace StoneSettings.ViewModels;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class AppsPageViewModel : ObservableObject
{
    private bool busy, available, loaded;
    private int count;
    public ObservableCollection<InstalledApp> Items { get; } = new();
    public bool IsIdle => !busy;
    public bool CanManagePackages => available && !busy;
    public string Summary => loaded ? UiText.Format(count == 1 ? "AppCountOne" : "AppCount", count) : UiText.Get("WaitingApps");
    internal Func<Task>? InstallRequested
    {
        get; set;
    }
    internal Func<InstalledApp, Task>? AppRequested
    {
        get; set;
    }
    internal Task InstallAsync() => InstallRequested?.Invoke() ?? throw new InvalidOperationException("The settings page has not been configured.");
    internal Task ShowAppAsync(InstalledApp app) => AppRequested?.Invoke(app) ?? throw new InvalidOperationException("The settings page has not been configured.");

    internal void Update(IEnumerable<InstalledApp> apps, bool isLoaded, bool canManage, int totalCount)
    {
        count = totalCount;
        loaded = isLoaded;
        available = canManage;
        Items.Clear();
        foreach (var app in apps)
            Items.Add(app);
        Notify(nameof(Summary));
        Notify(nameof(CanManagePackages));
    }

    internal void SetBusy(bool value)
    {
        busy = value;
        Notify(nameof(IsIdle));
        Notify(nameof(CanManagePackages));
    }
}
