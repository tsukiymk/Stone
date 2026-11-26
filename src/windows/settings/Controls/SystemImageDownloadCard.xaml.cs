using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using StoneSettings.Models;

namespace StoneSettings.Controls;

public sealed partial class SystemImageDownloadCard : UserControl
{
    public static readonly DependencyProperty IsPrimaryProperty = DependencyProperty.Register(nameof(IsPrimary),
        typeof(bool), typeof(SystemImageDownloadCard), new PropertyMetadata(false,
            (owner, _) => ((SystemImageDownloadCard)owner).UpdatePresentation()));
    public bool IsPrimary
    {
        get => (bool)GetValue(IsPrimaryProperty);
        set => SetValue(IsPrimaryProperty, value);
    }
    public static readonly DependencyProperty StateProperty = DependencyProperty.Register(nameof(State),
        typeof(SystemImageDownloadState), typeof(SystemImageDownloadCard), new PropertyMetadata(null,
            (owner, _) => ((SystemImageDownloadCard)owner).Bindings?.Update()));
    public SystemImageDownloadState State
    {
        get => (SystemImageDownloadState)GetValue(StateProperty);
        set => SetValue(StateProperty, value);
    }
    public event EventHandler? DownloadRequested;
    public event EventHandler? CancelRequested;
    public event EventHandler? UseRequested;
    public SystemImageDownloadCard()
    {
        State = new();
        InitializeComponent();
        UpdatePresentation();
    }
    private void UpdatePresentation()
    {
        if (DownloadButton is null)
            return;
        DownloadButton.Style = IsPrimary ? (Style)Application.Current.Resources["AccentButtonStyle"] : null;
        RecommendedBadge.Visibility = IsPrimary ? Visibility.Visible : Visibility.Collapsed;
    }
    private void DownloadClicked(object sender, RoutedEventArgs args) => DownloadRequested?.Invoke(this, EventArgs.Empty);
    private void CancelClicked(object sender, RoutedEventArgs args) => CancelRequested?.Invoke(this, EventArgs.Empty);
    private void UseClicked(object sender, RoutedEventArgs args) => UseRequested?.Invoke(this, EventArgs.Empty);
}
