using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;
using Windows.Graphics;

namespace StoneSettings.Controls;

public sealed partial class TitleBar : UserControl
{
    public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(
        nameof(Title), typeof(string), typeof(TitleBar), new PropertyMetadata(""));

    public static readonly DependencyProperty CenterContentProperty = DependencyProperty.Register(
        nameof(CenterContent), typeof(FrameworkElement), typeof(TitleBar), new PropertyMetadata(null));

    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public FrameworkElement? CenterContent
    {
        get => (FrameworkElement?)GetValue(CenterContentProperty);
        set => SetValue(CenterContentProperty, value);
    }

    public event RoutedEventHandler? PaneToggleRequested;

    internal Button PaneButton => PaneToggleButton;
    private Window? window;
    private XamlRoot? titleBarXamlRoot;

    public TitleBar() => InitializeComponent();

    internal void Configure(Window owner)
    {
        window = owner;
        owner.ExtendsContentIntoTitleBar = true;
        owner.AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        owner.SetTitleBar(this);
    }

    private void TogglePane(object sender, RoutedEventArgs args) => PaneToggleRequested?.Invoke(this, args);

    private void TitleBarLoaded(object sender, RoutedEventArgs args)
    {
        if (window is null || titleBarXamlRoot is not null)
            return;
        titleBarXamlRoot = XamlRoot;
        titleBarXamlRoot.Changed += TitleBarXamlRootChanged;
        window.AppWindow.Changed += TitleBarWindowChanged;
        Root.ActualThemeChanged += TitleBarThemeChanged;
        window.Closed += WindowClosed;
        UpdateLayoutAndInputRegions();
        UpdateCaptionButtons();
    }

    private void WindowClosed(object sender, WindowEventArgs args)
    {
        if (titleBarXamlRoot is not null)
            titleBarXamlRoot.Changed -= TitleBarXamlRootChanged;
        if (window is not null)
        {
            window.AppWindow.Changed -= TitleBarWindowChanged;
            window.Closed -= WindowClosed;
        }
        Root.ActualThemeChanged -= TitleBarThemeChanged;
        titleBarXamlRoot = null;
        window = null;
    }

    private void TitleBarSizeChanged(object sender, SizeChangedEventArgs args) => UpdateLayoutAndInputRegions();

    private void TitleBarXamlRootChanged(XamlRoot sender, XamlRootChangedEventArgs args) => UpdateLayoutAndInputRegions();

    private void TitleBarWindowChanged(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (args.DidSizeChange || args.DidPresenterChange)
            UpdateLayoutAndInputRegions();
    }

    private void TitleBarThemeChanged(FrameworkElement sender, object args) => UpdateCaptionButtons();

    private void UpdateLayoutAndInputRegions()
    {
        if (window is null || titleBarXamlRoot is null)
            return;
        double scale = titleBarXamlRoot.RasterizationScale;
        double leftInset = window.AppWindow.TitleBar.LeftInset / scale;
        double rightInset = window.AppWindow.TitleBar.RightInset / scale;
        // Equal side columns keep the content on the window centerline, even
        // when the native caption buttons occupy more space than the header.
        double sideWidth = Math.Max(leftInset + PaneToggleButton.ActualWidth + 12, rightInset + 8);
        LeftColumn.MinWidth = RightColumn.MinWidth = sideWidth;
        Header.Margin = new(leftInset, 0, 0, 0);
        DispatcherQueue.TryEnqueue(UpdateInputRegions);
    }

    private void UpdateInputRegions()
    {
        if (window is null || titleBarXamlRoot is null || CenterContent is null || !IsLoaded)
            return;
        double scale = titleBarXamlRoot.RasterizationScale;
        RectInt32 Bounds(FrameworkElement element)
        {
            Rect bounds = element.TransformToVisual(window.Content).TransformBounds(new Rect(0, 0, element.ActualWidth, element.ActualHeight));
            int left = (int)Math.Floor(bounds.Left * scale), top = (int)Math.Floor(bounds.Top * scale);
            return new(left, top, (int)Math.Ceiling(bounds.Right * scale) - left, (int)Math.Ceiling(bounds.Bottom * scale) - top);
        }
        InputNonClientPointerSource.GetForWindowId(window.AppWindow.Id).SetRegionRects(
            NonClientRegionKind.Passthrough, new[] { Bounds(PaneToggleButton), Bounds(CenterContent) });
    }

    private void UpdateCaptionButtons()
    {
        if (window is null)
            return;
        var titleBar = window.AppWindow.TitleBar;
        titleBar.ButtonBackgroundColor = Colors.Transparent;
        titleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
        titleBar.ButtonForegroundColor = Root.ActualTheme == ElementTheme.Dark ? Colors.White : Colors.Black;
        titleBar.ButtonInactiveForegroundColor = Colors.DarkGray;
    }
}
