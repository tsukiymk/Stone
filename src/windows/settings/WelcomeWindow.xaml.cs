using Microsoft.UI.Xaml;
using StoneSettings.Helpers;
using StoneSettings.Utils;

namespace StoneSettings;

public sealed partial class WelcomeWindow : Window
{
    internal event Action<string>? ImageSelected;

    internal WelcomeWindow(string dataRoot)
    {
        InitializeComponent();
        Title = UiText.Get("WelcomeTitle");
        Welcome.Configure(this, dataRoot);
        Welcome.ImageSelected += image => ImageSelected?.Invoke(image);
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(Welcome.TitleBar);
        WindowSizing.Resize(this, 680, 720);
    }
}
