using Microsoft.UI.Xaml;
using StoneSettings.Helpers;

namespace StoneSettings;

public sealed partial class MainWindow : Window
{
    internal MainWindow(string? selectedImage)
    {
        InitializeComponent();
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "Stone.ico"));
        Shell.Configure(this, selectedImage);
        Title = UiText.Get("SettingsTitle");
    }
}
