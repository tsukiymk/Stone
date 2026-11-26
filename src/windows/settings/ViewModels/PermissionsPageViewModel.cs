using System.Collections.ObjectModel;
using Microsoft.UI.Xaml;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;

namespace StoneSettings.ViewModels;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class PermissionsPageViewModel : ObservableObject
{
    private string dataRoot = "";
    private bool busy;
    private int selectionVersion;
    private Func<NotificationPolicy> getPolicy = null!;
    private Action<bool> setBusy = null!;
    private Func<string?, bool, Task> saveNotification = null!;
    private Func<string, Exception, Task> showError = null!;
    public ObservableCollection<InstalledApp> Items { get; } = new();
    public InstalledApp? SelectedApp
    {
        get; private set;
    }
    public bool IsIdle => !busy;
    public bool IsLoading
    {
        get; private set;
    }
    public bool IsSavingStorage
    {
        get; private set;
    }
    private bool savingNotification;
    public bool CardsEnabled => IsIdle && !IsLoading && !IsSavingStorage && !savingNotification && SelectedApp is not null;
    public Visibility CardsVisibility => SelectedApp is null ? Visibility.Collapsed : Visibility.Visible;
    public Visibility EmptyVisibility => SelectedApp is null ? Visibility.Visible : Visibility.Collapsed;
    public string PackageName => SelectedApp?.PackageName ?? "";
    public bool NotificationAllowed
    {
        get; private set;
    }
    public bool StorageAllowed
    {
        get; private set;
    }
    public bool NotificationEnabled => SelectedApp?.RequestsNotifications == true && CardsEnabled;
    public bool StorageEnabled => SelectedApp?.RequestsAllFiles == true && !string.IsNullOrEmpty(SelectedApp.StorageIdentity) && CardsEnabled;
    public string NotificationDescription { get; private set; } = "";
    public string StorageDescription { get; private set; } = "";
    public string DeclaredPermissions { get; private set; } = "";

    internal void Configure(string root, Func<NotificationPolicy> policy, Action<bool> activity,
        Func<string?, bool, Task> saveNotifications, Func<string, Exception, Task> error)
    {
        dataRoot = root;
        getPolicy = policy;
        setBusy = activity;
        saveNotification = saveNotifications;
        showError = error;
    }

    internal void Update(IEnumerable<InstalledApp> apps)
    {
        Items.Clear();
        foreach (var app in apps)
            Items.Add(app);
    }

    internal void SetBusy(bool value)
    {
        busy = value;
        NotifyInteraction();
    }

    internal async Task SelectAsync(InstalledApp? app)
    {
        int version = ++selectionVersion;
        SelectedApp = app;
        IsLoading = true;
        NotifyState();
        try
        {
            if (app is null)
                return;
            bool allowed = await Task.Run(() => StorageAccessPolicy.Load(dataRoot, app));
            if (version != selectionVersion || !ReferenceEquals(SelectedApp, app))
                return;
            NotificationAllowed = getPolicy().Allowed(app.PackageName);
            StorageAllowed = allowed;
            NotificationDescription = UiText.Get(app.RequestsNotifications ? "NotificationGrantedDescription" : "NotificationUndeclared");
            StorageDescription = UiText.Get(app.RequestsAllFiles ? "StorageGrantedDescription" : "StorageUndeclared");
            DeclaredPermissions = app.RequestedPermissions.Length == 0 ? UiText.Get("NoPermissions")
                : string.Join("\n", app.RequestedPermissions.Order(StringComparer.Ordinal));
        }
        catch (Exception error)
        {
            if (version == selectionVersion)
                await showError(UiText.Get("ReadPermissionsFailed"), error);
        }
        finally
        {
            if (version == selectionVersion)
            {
                IsLoading = false;
                NotifyState();
            }
        }
    }

    internal async Task SetNotificationAsync(bool allowed)
    {
        if (IsLoading || savingNotification || !IsIdle || SelectedApp is not InstalledApp app || !app.RequestsNotifications
            || allowed == getPolicy().Allowed(app.PackageName))
            return;
        savingNotification = true;
        NotifyInteraction();
        try
        {
            await saveNotification(app.PackageName, allowed);
        }
        finally
        {
            if (ReferenceEquals(SelectedApp, app))
                await SelectAsync(app);
            savingNotification = false;
            NotifyInteraction();
        }
    }

    internal async Task SetStorageAsync(bool allowed)
    {
        if (IsLoading || IsSavingStorage || !IsIdle || SelectedApp is not InstalledApp app
            || !app.RequestsAllFiles || string.IsNullOrEmpty(app.StorageIdentity) || allowed == StorageAllowed)
            return;
        IsSavingStorage = true;
        setBusy(true);
        try
        {
            await StorageAccessPolicy.SetAsync(dataRoot, app, allowed);
        }
        catch (Exception error)
        {
            await showError(UiText.Get("SaveStorageFailed"), error);
        }
        finally
        {
            setBusy(false);
            if (ReferenceEquals(SelectedApp, app))
                await SelectAsync(app);
            IsSavingStorage = false;
            NotifyInteraction();
        }
    }

    private void NotifyState()
    {
        foreach (string property in new[] { nameof(IsIdle), nameof(IsLoading), nameof(IsSavingStorage), nameof(CardsEnabled),
            nameof(CardsVisibility), nameof(EmptyVisibility), nameof(PackageName), nameof(NotificationAllowed), nameof(StorageAllowed),
            nameof(NotificationEnabled), nameof(StorageEnabled), nameof(NotificationDescription), nameof(StorageDescription), nameof(DeclaredPermissions) })
            Notify(property);
    }

    private void NotifyInteraction()
    {
        foreach (string property in new[] { nameof(IsIdle), nameof(IsSavingStorage), nameof(CardsEnabled), nameof(NotificationEnabled), nameof(StorageEnabled) })
            Notify(property);
    }
}
