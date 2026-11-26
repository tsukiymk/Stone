

namespace StoneSettings.Utils;

internal static class RuntimeLayout
{
    // Settings and its dependencies live one level below the native host and image.
    internal static string Directory => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    internal static string StoneExecutable => Path.Combine(Directory, "StoneClient.exe");
    internal static string ServiceExecutable => Path.Combine(Directory, "StoneService.exe");
    internal static string SystemImage => Path.Combine(Directory, "system.img");
}
