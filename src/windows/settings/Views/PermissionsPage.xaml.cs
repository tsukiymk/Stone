using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using StoneSettings.Models;
using StoneSettings.ViewModels;

namespace StoneSettings.Views;

public sealed partial class PermissionsPage : Page
{
    public PermissionsPageViewModel ViewModel
    {
        get;
    }

    internal PermissionsPage(PermissionsPageViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        Bindings.Update();
    }

    internal void UpdateBindings() => Bindings.Update();

    private async void PermissionAppChanged(object sender, SelectionChangedEventArgs args) =>
        await ViewModel.SelectAsync(PermissionAppSelector.SelectedItem as InstalledApp);

    private async void PermissionNotificationsToggled(object sender, RoutedEventArgs args) =>
        await ViewModel.SetNotificationAsync(PermissionNotifications.IsOn);

    private async void PermissionAllFilesToggled(object sender, RoutedEventArgs args) =>
        await ViewModel.SetStorageAsync(PermissionAllFiles.IsOn);
}
