using System.Runtime.InteropServices;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.Graphics;

namespace StoneSettings.Utils;

internal static partial class PermissionDialog
{
    [LibraryImport("user32.dll")]
    private static partial nint GetForegroundWindow();

    internal static DisplayArea GetTargetDisplay() => DisplayArea.GetFromWindowId(
        Microsoft.UI.Win32Interop.GetWindowIdFromWindow(GetForegroundWindow()), DisplayAreaFallback.Primary);

    [LibraryImport("dwmapi.dll")]
    private static partial int DwmSetWindowAttribute(nint window, uint attribute, in int value, uint size);

    internal static void Configure(Window window, DisplayArea display)
    {
        if (window.AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.SetBorderAndTitleBar(false, false);
            presenter.IsResizable = presenter.IsMaximizable = presenter.IsMinimizable = false;
        }
        RectInt32 work = display.WorkArea;
        window.AppWindow.Move(new(work.X, work.Y));
        WindowSizing.Resize(window, 548, 280);
        int rounded = 2; // DWMWCP_ROUND; older Windows versions ignore it.
        DwmSetWindowAttribute(WinRT.Interop.WindowNative.GetWindowHandle(window), 33, in rounded, sizeof(int));
        SizeInt32 size = window.AppWindow.Size;
        window.AppWindow.Move(new(work.X + (work.Width - size.Width) / 2, work.Y + (work.Height - size.Height) / 2));
        window.AppWindow.IsShownInSwitchers = true;
    }
}
