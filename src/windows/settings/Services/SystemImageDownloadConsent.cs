using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using StoneSettings.Helpers;

namespace StoneSettings.Services;

internal static class SystemImageDownloadConsent
{
    internal static async Task<bool> ShowAsync(XamlRoot root, SystemImagePackage package)
    {
        StackPanel content = new() { Spacing = 12 };
        content.Children.Add(new TextBlock
        {
            Text = UiText.Get("ImageDownloadConsentDescription"),
            TextWrapping = TextWrapping.Wrap
        });
        content.Children.Add(new ScrollViewer
        {
            Height = 260, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = new TextBlock { Text = package.License, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true }
        });
        ContentDialog dialog = new()
        {
            XamlRoot = root, Title = UiText.Get("ImageDownloadConsentTitle"), Content = content,
            PrimaryButtonText = UiText.Get("ImageDownloadAccept"), CloseButtonText = UiText.Get("Cancel"),
            DefaultButton = ContentDialogButton.Close
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }
}
