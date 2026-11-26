using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using StoneSettings.ViewModels;
using Windows.Storage.Pickers;
using StoneSettings.Services;

namespace StoneSettings.Views;

public sealed partial class WelcomePage : Page
{
    private Window owner = null!;
    public WelcomePageViewModel ViewModel { get; } = new();
    internal FrameworkElement TitleBar => WindowTitleBar;
    internal event Action<string>? ImageSelected;

    public WelcomePage() => InitializeComponent();

    internal void Configure(Window window, string root)
    {
        owner = window;
        owner.Closed += (_, _) => ViewModel.CancelDownload();
        ViewModel.Configure(root);
    }

    private async void ChooseImageClicked(object sender, RoutedEventArgs args)
    {
        if (!ViewModel.CanChooseImage)
            return;
        ViewModel.SetPicking(true);
        try
        {
            FileOpenPicker picker = new();
            picker.FileTypeFilter.Add(".img");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(owner));
            var file = await picker.PickSingleFileAsync();
            if (file is not null)
                await UseImageAsync(file.Path);
        }
        catch (Exception error)
        {
            ViewModel.ReportError(error);
        }
        finally
        {
            ViewModel.SetPicking(false);
        }
    }

    private async Task<bool> UseImageAsync(string image)
    {
        if (!await ViewModel.UseImageAsync(image))
            return false;
        ImageSelected?.Invoke(image);
        return true;
    }

    private async void DownloadRequested(object? sender, EventArgs args) => await ViewModel.DownloadImageAsync(
        package => SystemImageDownloadConsent.ShowAsync(Root.XamlRoot, package));
    private void CancelRequested(object? sender, EventArgs args) => ViewModel.CancelDownload();
    private async void UseRequested(object? sender, EventArgs args)
    {
        if (ViewModel.Download.CanUse)
            await UseImageAsync(ViewModel.Download.ImagePath);
    }
}
