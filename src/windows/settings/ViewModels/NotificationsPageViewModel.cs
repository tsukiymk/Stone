using System.Collections.ObjectModel;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;

namespace StoneSettings.ViewModels;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class NotificationsPageViewModel : ObservableObject
{
    private bool busy, ready, global = true;
    private NotificationPolicy policy = new();
    public ObservableCollection<InstalledApp> Items { get; } = new();
    public bool IsIdle => !busy;
    public bool GlobalEnabled => global;
    internal Func<string?, bool, Task>? SaveRequested
    {
        get; set;
    }
    internal Func<Task>? OpenWindowsRequested
    {
        get; set;
    }
    internal void Activate() => ready = true;
    internal Task OpenWindowsAsync() => OpenWindowsRequested?.Invoke() ?? throw new InvalidOperationException("The settings page has not been configured.");
    internal Task SetGlobalAsync(bool allowed) => !ready || busy || global == allowed
        ? Task.CompletedTask : SaveRequested?.Invoke(null, allowed) ?? throw new InvalidOperationException("The settings page has not been configured.");
    internal Task SetAppAsync(InstalledApp app, bool allowed) => !ready || busy || policy.Allowed(app.PackageName) == allowed
        ? Task.CompletedTask : SaveRequested?.Invoke(app.PackageName, allowed) ?? throw new InvalidOperationException("The settings page has not been configured.");

    internal void ApplyPolicy(NotificationPolicy value)
    {
        policy = value;
        global = value.Global;
        foreach (var app in Items)
            app.NotificationsEnabled = value.Allowed(app.PackageName);
        Notify(nameof(GlobalEnabled));
    }

    internal void Update(IEnumerable<InstalledApp> apps)
    {
        Items.Clear();
        foreach (var app in apps)
        {
            app.NotificationsEnabled = policy.Allowed(app.PackageName);
            Items.Add(app);
        }
    }

    internal void SetBusy(bool value)
    {
        busy = value;
        Notify(nameof(IsIdle));
    }
}
