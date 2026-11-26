using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;
using StoneSettings.Utils;
using StoneSettings.ViewModels;
using Windows.Storage.Pickers;
using Windows.System;

namespace StoneSettings.Views;

public sealed partial class ShellPage : Page
{
    private Window owner = null!;
    public ShellPageViewModel ViewModel { get; } = new();
    private ShellPageViewModel State => ViewModel;
    private SystemPage systemPage = null!;
    private AppsPage appsPage = null!;
    private NotificationsPage notificationsPage = null!;
    private PermissionsPage permissionsPage = null!;
    private InstalledApp? permissionApp => State.Permissions.SelectedApp;

    public ShellPage() => InitializeComponent();

    private void InitializePages()
    {
        State.Apps.InstallRequested = InstallAsync;
        State.Apps.AppRequested = ShowAppDetailsAsync;
        State.System.SelectImageRequested = ChooseImageAsync;
        State.System.DownloadImageRequested = () => State.System.Downloader.DownloadAsync(
            package => SystemImageDownloadConsent.ShowAsync(Root.XamlRoot, package), State.SetBusy);
        State.System.UseDownloadedImageRequested = UseDownloadedImageAsync;
        State.Notifications.SaveRequested = SaveNotificationPolicyAsync;
        State.Notifications.OpenWindowsRequested = OpenWindowsNotificationsAsync;
        State.Permissions.Configure(dataRoot, () => notificationPolicy, State.SetBusy, SaveNotificationPolicyAsync, ShowErrorAsync);
        systemPage = new(State.System);
        appsPage = new(State.Apps);
        notificationsPage = new(State.Notifications);
        permissionsPage = new(State.Permissions);
    }

    private string dataRoot = "";
    private bool ready;
    private bool dialogOpen;
    private SemaphoreSlim dialogs = new(1, 1);
    private bool refreshing, operating;
    private string? systemImage;
    private PackageManagerClient packageManager = null!;
    private NotificationPolicy notificationPolicy = new();
    private bool savingNotificationPolicy;


    private ProgressRing SystemLoadingRing => systemPage.SystemLoadingRing;
    private TextBlock AppsListHeading => appsPage.AppsListHeading;
    private StackPanel EmptyApps => appsPage.EmptyApps;
    private TextBlock EmptyTitle => appsPage.EmptyTitle;
    private TextBlock EmptyDescription => appsPage.EmptyDescription;
    private Grid AppsLoadingPanel => appsPage.AppsLoadingPanel;
    private ProgressRing AppsLoadingRing => appsPage.AppsLoadingRing;
    private ComboBox PermissionAppSelector => permissionsPage.PermissionAppSelector;

    internal void Configure(Window window, string? selectedImage)
    {
        owner = window;
        owner.Closed += (_, _) => State.System.CancelDownload();
        WindowTitleBar.Configure(window);
        string[] args = Environment.GetCommandLineArgs();
        systemImage = selectedImage;
        packageManager = new(RuntimeLayout.StoneExecutable);
        dataRoot = SystemImageSelection.DataRoot(args);
        InitializePages();
        UpdateSystemImage();
        Navigation.SelectedItem = AppsNavigation;
        KeyboardAccelerator find = new()
        {
            Key = VirtualKey.F,
            Modifiers = VirtualKeyModifiers.Control
        };
        find.Invoked += (_, e) =>
        {
            SearchBox.Focus(FocusState.Programmatic);
            e.Handled = true;
        };
        Root.KeyboardAccelerators.Add(find);
        Root.Loaded += async (_, _) =>
        {
            if (ready)
                return;
            ready = true;
            await RefreshAsync();
        };
    }

    private void TogglePane(object sender, RoutedEventArgs args) => Navigation.IsPaneOpen = !Navigation.IsPaneOpen;

    private void NavigationChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (appsPage is null)
            return;
        bool notifications = ReferenceEquals(sender.SelectedItem, NotificationsNavigation);
        bool system = ReferenceEquals(sender.SelectedItem, SystemNavigation);
        bool permissions = ReferenceEquals(sender.SelectedItem, PermissionsNavigation);
        PageFrame.Content = system ? systemPage : notifications ? notificationsPage : permissions ? permissionsPage : appsPage;
        if (system)
            systemPage.UpdateBindings();
        else if (notifications)
            notificationsPage.UpdateBindings();
        else if (permissions)
            permissionsPage.UpdateBindings();
        else
            appsPage.UpdateBindings();
        PageTitle.Text = UiText.Get(system ? "SystemNavigation" : notifications ? "NotificationsNavigation" : permissions ? "PermissionsNavigation" : "AppsNavigation");
        PageDescription.Text = UiText.Get(system ? "SystemDescription" : notifications ? "NotificationsDescription" : permissions ? "PermissionsDescription" : "AppsDescription");
        if (permissions && PermissionAppSelector.SelectedIndex < 0 && State.VisiblePermissionApps.Count > 0)
            PermissionAppSelector.SelectedIndex = 0;
        else if (permissions && permissionApp is not null)
            _ = LoadPermissionAppAsync(permissionApp);
    }

    private async Task RefreshAsync()
    {
        if (refreshing || operating)
            return;
        refreshing = true;
        State.SetBusy(true);
        SetAppsLoading(true);
        try
        {
            try
            {
                await State.LoadAsync(dataRoot);
            }
            catch (Exception error)
            {
                System.Diagnostics.Trace.WriteLine(error);
            }
            if (System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture != System.Runtime.InteropServices.Architecture.X64)
                throw new NotSupportedException(UiText.Get("Arm64Unavailable"));
            if (systemImage is null)
                throw new IOException(UiText.Get("SelectImageFirst"));
            await SystemImageSelection.ValidateAsync(packageManager, dataRoot, systemImage);
            await State.LoadAsync(dataRoot, true);
        }
        catch (Exception error)
        {
            await ShowErrorAsync(UiText.Get("ReadAppsFailed"), error);
        }
        finally
        {
            try
            {
                ApplyNotificationPolicy(await Task.Run(() => NotificationPolicy.Load(dataRoot)));
            }
            catch (Exception error)
            {
                await ShowErrorAsync(UiText.Get("ReadNotificationsFailed"), error);
            }
            refreshing = false;
            State.SetBusy(false);
            SetAppsLoading(false);
            UpdateEmptyState();
        }
    }

    private void ApplyNotificationPolicy(NotificationPolicy policy)
    {
        notificationPolicy = policy;
        State.ApplyNotificationPolicy(policy);
    }

    private Task LoadPermissionAppAsync(InstalledApp? app) => State.Permissions.SelectAsync(app);

    private async Task SaveNotificationPolicyAsync(string? package, bool allowed)
    {
        if (savingNotificationPolicy || operating || refreshing)
        {
            ApplyNotificationPolicy(notificationPolicy);
            return;
        }
        savingNotificationPolicy = true;
        State.SetBusy(true);
        try
        {
            ApplyNotificationPolicy(await NotificationPolicy.SetAsync(dataRoot, package, allowed));
        }
        catch (Exception error)
        {
            ApplyNotificationPolicy(notificationPolicy);
            await ShowErrorAsync(UiText.Get("SaveNotificationsFailed"), error);
        }
        finally
        {
            savingNotificationPolicy = false;
            State.SetBusy(false);
        }
    }

    private void UpdateSystemImage() => State.System.ImagePath = systemImage ?? UiText.Get("NoImage");

    private void SetAppsLoading(bool loading)
    {
        AppsLoadingRing.IsActive = loading;
        AppsLoadingPanel.Visibility = loading ? Visibility.Visible : Visibility.Collapsed;
        EmptyApps.Visibility = loading || State.VisibleApps.Count != 0 ? Visibility.Collapsed : Visibility.Visible;
    }

    private void UpdateEmptyState()
    {
        EmptyApps.Visibility = !AppsLoadingRing.IsActive && State.VisibleApps.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        bool filtered = !string.IsNullOrWhiteSpace(SearchBox.Text);
        EmptyTitle.Text = filtered ? UiText.Get("NoMatchingApps") : State.Loaded ? UiText.Get("NoApps") : UiText.Get("CatalogNotLoaded");
        EmptyDescription.Text = filtered ? UiText.Get("TryAppName") : State.Loaded ? UiText.Get("InstallFirstApp") : UiText.Get("CheckImage");
        AppsListHeading.Text = filtered ? UiText.Get("SearchResults") : UiText.Get("AllApps");
    }

    private void SearchChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
    {
        if (!ready)
            return;
        State.Filter(sender.Text);
        sender.ItemsSource = string.IsNullOrWhiteSpace(sender.Text) ? Array.Empty<string>() : State.Suggestions(sender.Text);
        UpdateEmptyState();
    }

    private void SearchChosen(AutoSuggestBox sender, AutoSuggestBoxSuggestionChosenEventArgs args) => sender.Text = (string)args.SelectedItem;

    private void SearchSubmitted(AutoSuggestBox sender, AutoSuggestBoxQuerySubmittedEventArgs args)
    {
        string query = args.ChosenSuggestion as string ?? sender.Text;
        if (query == UiText.Get("SystemNavigation") || query == UiText.Get("NotificationsNavigation") || query == UiText.Get("AppsNavigation") || query == UiText.Get("PermissionsNavigation"))
        {
            Navigation.SelectedItem = query == UiText.Get("SystemNavigation") ? SystemNavigation : query == UiText.Get("NotificationsNavigation") ? NotificationsNavigation : query == UiText.Get("PermissionsNavigation") ? PermissionsNavigation : AppsNavigation;
            sender.Text = "";
        }
        else
        {
            State.Filter(query);
            UpdateEmptyState();
        }
    }

    private async Task ShowErrorAsync(string title, Exception error)
    {
        System.Diagnostics.Trace.WriteLine(error);
        ContentDialog dialog = new()
        {
            XamlRoot = Root.XamlRoot,
            Title = title,
            Content = error.Message,
            CloseButtonText = UiText.Get("Close"),
            DefaultButton = ContentDialogButton.Close
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(dialog, "SettingsErrorDialog");
        await dialogs.WaitAsync();
        try
        {
            await dialog.ShowAsync();
        }
        finally
        {
            dialogs.Release();
        }
    }

    private async Task OpenWindowsNotificationsAsync()
    {
        try
        {
            await Launcher.LaunchUriAsync(new Uri("ms-settings:notifications"));
        }
        catch (Exception error)
        {
            await ShowErrorAsync(UiText.Get("OpenNotificationsFailed"), error);
        }
    }

    private async Task InstallAsync()
    {
        try
        {
            FileOpenPicker picker = new();
            picker.FileTypeFilter.Add(".apk");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(owner));
            var file = await picker.PickSingleFileAsync();
            if (file is not null)
                await RunPackageOperationAsync("install", file.Path);
        }
        catch (Exception error)
        {
            await ShowErrorAsync(UiText.Get("InstallFailed"), error);
        }
    }

    private async Task ChooseImageAsync()
    {
        if (!State.IsIdle)
            return;
        try
        {
            FileOpenPicker picker = new();
            picker.FileTypeFilter.Add(".img");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(owner));
            var file = await picker.PickSingleFileAsync();
            if (file is not null)
                await ChangeSystemImageAsync(file.Path);
        }
        catch (Exception error)
        {
            await ShowErrorAsync(UiText.Get("ChooseImageFailed"), error);
        }
    }

    private async Task UseDownloadedImageAsync()
    {
        if (!State.System.Download.CanUse)
            return;
        try { await ChangeSystemImageAsync(State.System.Download.ImagePath); }
        catch (Exception error) { await ShowErrorAsync(UiText.Get("UseImageFailed"), error); }
    }

    private async Task ChangeSystemImageAsync(string image)
    {
        if (!State.IsIdle)
            return;
        refreshing = true;
        State.SetBusy(true);
        SetAppsLoading(true);
        SystemLoadingRing.Visibility = Visibility.Visible;
        SystemLoadingRing.IsActive = true;
        try
        {
            await SystemImageSelection.ValidateAsync(packageManager, dataRoot, image);
            systemImage = image;
            UpdateSystemImage();
            await State.LoadAsync(dataRoot, true);
            ApplyNotificationPolicy(await Task.Run(() => NotificationPolicy.Load(dataRoot)));
        }
        finally
        {
            refreshing = false;
            State.SetBusy(false);
            SetAppsLoading(false);
            UpdateEmptyState();
            SystemLoadingRing.IsActive = false;
            SystemLoadingRing.Visibility = Visibility.Collapsed;
        }
    }


    private async Task ShowAppDetailsAsync(InstalledApp app)
    {
        if (dialogOpen)
            return;
        dialogOpen = true;
        try
        {
            StackPanel details = new()
            {
                Spacing = 12
            };
            Grid applicationActions = new() { ColumnSpacing = 24 };
            applicationActions.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            applicationActions.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
            applicationActions.Children.Add(new Controls.ApplicationIcon { Width = 64, Height = 64,
                HorizontalAlignment = HorizontalAlignment.Left, IconPng = app.IconPng });
            Button open = new() { Content = UiText.Get("OpenApp"), VerticalAlignment = VerticalAlignment.Center,
                IsEnabled = State.CanManagePackages, Style = (Style)Application.Current.Resources["AccentButtonStyle"] };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(open, "OpenApp");
            Grid.SetColumn(open, 1);
            applicationActions.Children.Add(open);
            details.Children.Add(applicationActions);
            details.Children.Add(new TextBlock { Text = app.PackageName, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true });
            details.Children.Add(new TextBlock { Text = UiText.Format("Version", app.Version) });
            details.Children.Add(new TextBlock
            {
                Text = UiText.Format("InstallSource", app.Installer == "com.trebledot.stone.settings" ? UiText.Get("SettingsTitle")
                : string.IsNullOrEmpty(app.Installer) ? UiText.Get("ApkFile") : app.Installer),
                TextWrapping = TextWrapping.Wrap
            });
            if (app.CodeBytes > 0)
                details.Children.Add(new TextBlock { Text = UiText.Format("AppSize", app.CodeBytes / 1024d / 1024) });
            ContentDialog dialog = new()
            {
                XamlRoot = Root.XamlRoot,
                Title = app.Name,
                Content = details,
                PrimaryButtonText = UiText.Get("Uninstall"),
                IsPrimaryButtonEnabled = State.CanManagePackages,
                SecondaryButtonText = UiText.Get("ManagePermissions"),
                CloseButtonText = UiText.Get("Done"),
                DefaultButton = ContentDialogButton.None
            };
            ContentDialogResult decision;
            Exception? launchError = null;
            open.Click += (_, _) =>
            {
                open.IsEnabled = false;
                try { ApplicationLauncher.Open(RuntimeLayout.StoneExecutable, app.PackageName, dataRoot, systemImage); }
                catch (Exception error) { launchError = error; }
                dialog.Hide();
            };
            await dialogs.WaitAsync();
            try
            {
                decision = await dialog.ShowAsync();
            }
            finally
            {
                dialogs.Release();
            }
            if (launchError is not null)
            {
                await ShowErrorAsync(UiText.Get("OpenAppFailed"), launchError);
                return;
            }
            if (decision == ContentDialogResult.Primary)
                await RunPackageOperationAsync("uninstall", app.PackageName);
            else if (decision == ContentDialogResult.Secondary)
            {
                Navigation.SelectedItem = PermissionsNavigation;
                PermissionAppSelector.SelectedItem = State.VisiblePermissionApps.FirstOrDefault(value => value.PackageName == app.PackageName);
            }
        }
        catch (Exception error)
        {
            await ShowErrorAsync(UiText.Get("ManageAppFailed"), error);
        }
        finally
        {
            dialogOpen = false;
        }
    }

    private async Task RunPackageOperationAsync(string operation, string target)
    {
        if (!State.CanManagePackages)
            throw new NotSupportedException("The settings package-management endpoint is unavailable.");
        if (operating || refreshing)
            return;
        operating = true;
        State.SetBusy(true);
        try
        {
            PackageOperationResult result = operation == "install"
                ? await new InstallWindow(target).RunAsync(packageManager, dataRoot, systemImage)
                : await packageManager.ExecuteAsync(operation, target, dataRoot, systemImage);
            SetAppsLoading(true);
            await State.LoadAsync(dataRoot, true);
            if (result.Status == "failed" && operation != "install")
                throw new IOException(result.Message);
        }
        finally
        {
            operating = false;
            State.SetBusy(false);
            SetAppsLoading(false);
            UpdateEmptyState();
        }
    }

}
