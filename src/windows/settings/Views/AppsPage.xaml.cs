using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using StoneSettings.Models;
using StoneSettings.ViewModels;

namespace StoneSettings.Views;

public sealed partial class AppsPage : Page
{
    public AppsPageViewModel ViewModel
    {
        get;
    }

    internal AppsPage(AppsPageViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        Bindings.Update();
    }

    internal void UpdateBindings() => Bindings.Update();

    private async void InstallClicked(object sender, RoutedEventArgs args) => await ViewModel.InstallAsync();
    private async void AppClicked(object sender, ItemClickEventArgs args) => await ViewModel.ShowAppAsync((InstalledApp)args.ClickedItem);
}
