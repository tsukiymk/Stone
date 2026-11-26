using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using StoneSettings.Models;
using StoneSettings.ViewModels;

namespace StoneSettings.Views;

public sealed partial class NotificationsPage : Page
{
    public NotificationsPageViewModel ViewModel
    {
        get;
    }

    internal NotificationsPage(NotificationsPageViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        Bindings.Update();
        ViewModel.Activate();
    }

    internal void UpdateBindings() => Bindings.Update();

    private async void AllNotificationsToggled(object sender, RoutedEventArgs args) => await ViewModel.SetGlobalAsync(AllNotifications.IsOn);

    private async void AppNotificationsToggled(object sender, RoutedEventArgs args)
    {
        if (sender is ToggleSwitch toggle && toggle.DataContext is InstalledApp app)
            await ViewModel.SetAppAsync(app, toggle.IsOn);
    }

    private async void WindowsNotificationsClicked(object sender, RoutedEventArgs args) => await ViewModel.OpenWindowsAsync();
}
