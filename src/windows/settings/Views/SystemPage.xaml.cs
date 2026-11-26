using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using StoneSettings.ViewModels;

namespace StoneSettings.Views;

public sealed partial class SystemPage : Page
{
    public SystemPageViewModel ViewModel
    {
        get;
    }

    internal SystemPage(SystemPageViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        Bindings.Update();
    }

    internal void UpdateBindings() => Bindings.Update();

    private async void ChooseImageClicked(object sender, RoutedEventArgs args) => await ViewModel.SelectImageAsync();
    private async void DownloadRequested(object? sender, EventArgs args) => await ViewModel.DownloadImageAsync();
    private void CancelRequested(object? sender, EventArgs args) => ViewModel.CancelDownload();
    private async void UseRequested(object? sender, EventArgs args) => await ViewModel.UseDownloadedImageAsync();
}
