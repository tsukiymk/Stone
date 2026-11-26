using System.Collections.ObjectModel;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;

namespace StoneSettings.ViewModels;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class ShellPageViewModel : ObservableObject
{
    private readonly ApplicationCatalogService catalog = new();
    private ApplicationSnapshot snapshot = new(false, false, [], []);
    private string query = "";
    private bool busy;
    public AppsPageViewModel Apps { get; } = new();
    public SystemPageViewModel System { get; } = new();
    public NotificationsPageViewModel Notifications { get; } = new();
    public PermissionsPageViewModel Permissions { get; } = new();
    public ObservableCollection<InstalledApp> VisibleApps => Apps.Items;
    public ObservableCollection<InstalledApp> VisibleNotificationApps => Notifications.Items;
    public ObservableCollection<InstalledApp> VisiblePermissionApps => Permissions.Items;
    public bool Loaded => snapshot.Loaded;
    public bool IsIdle => !busy;
    public bool CanManagePackages => Apps.CanManagePackages;
    public string Summary => Apps.Summary;

    public async Task LoadAsync(string dataRoot, bool backendAvailable = false)
    {
        snapshot = await catalog.LoadAsync(dataRoot, backendAvailable);
        Filter(query);
    }

    public void SetBusy(bool value)
    {
        busy = value;
        Apps.SetBusy(value);
        System.SetBusy(value);
        Notifications.SetBusy(value);
        Permissions.SetBusy(value);
        Notify(nameof(IsIdle));
        Notify(nameof(CanManagePackages));
    }

    public void Filter(string text)
    {
        query = text.Trim();
        Apps.Update(snapshot.Apps.Where(app => app.Matches(query)), snapshot.Loaded, snapshot.CanManagePackages, snapshot.Apps.Count);
        Notifications.Update(snapshot.NotificationApps.Where(app => app.Matches(query)));
        Permissions.Update(snapshot.NotificationApps.Where(app => app.Matches(query)));
        Notify(nameof(Summary));
        Notify(nameof(Loaded));
        Notify(nameof(CanManagePackages));
    }

    public string[] Suggestions(string text) =>
        new[] { UiText.Get("SystemNavigation"), UiText.Get("AppsNavigation"), UiText.Get("NotificationsNavigation"), UiText.Get("PermissionsNavigation") }
            .Where(name => name.Contains(text, StringComparison.CurrentCultureIgnoreCase))
            .Concat(snapshot.Apps.Where(app => app.Matches(text)).Take(6).Select(app => app.Name)).ToArray();

    internal void ApplyNotificationPolicy(NotificationPolicy policy)
    {
        foreach (var app in snapshot.NotificationApps)
            app.NotificationsEnabled = policy.Allowed(app.PackageName);
        Notifications.ApplyPolicy(policy);
    }
}
