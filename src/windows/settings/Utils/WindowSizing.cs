using System.Runtime.InteropServices;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.Graphics;

namespace StoneSettings.Utils;

internal static partial class WindowSizing
{
    [LibraryImport("user32.dll")]
    private static partial uint GetDpiForWindow(nint window);

    private static uint Dpi(Window window)
    {
        uint dpi = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(window));
        return dpi == 0 ? 96u : dpi;
    }

    internal static SizeInt32 Pixels(int width, int height, uint dpi, SizeInt32 workArea) => new(
        Math.Min((int)Math.Round(width * dpi / 96d, MidpointRounding.AwayFromZero), workArea.Width),
        Math.Min((int)Math.Round(height * dpi / 96d, MidpointRounding.AwayFromZero), workArea.Height));

    private static SizeInt32 WorkArea(Window window)
    {
        var area = DisplayArea.GetFromWindowId(window.AppWindow.Id, DisplayAreaFallback.Nearest).WorkArea;
        return new(area.Width, area.Height);
    }

    // XAML sizes are DIPs; AppWindow.Resize takes physical screen pixels.
    // Apply this once at creation. PerMonitorV2 handles subsequent DPI changes.
    internal static void Resize(Window window, int width, int height)
    {
        window.AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "Stone.ico"));
        window.AppWindow.Resize(Pixels(width, height, Dpi(window), WorkArea(window)));
    }
}
