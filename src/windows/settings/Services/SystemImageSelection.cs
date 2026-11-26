using StoneSettings.Models;
using StoneSettings.Utils;

namespace StoneSettings.Services;

internal static class SystemImageSelection
{
    internal static string DataRoot(string[] args)
    {
        int flag = Array.IndexOf(args, "--data-dir");
        return Path.GetFullPath(flag >= 0 && flag + 1 < args.Length ? args[flag + 1]
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Trebledot Productions", "Stone", "User Data"));
    }

    internal static async Task<string?> ResolveAsync(string[] args, string dataRoot)
    {
        int flag = Array.IndexOf(args, "--system-image");
        string? image = flag >= 0 && flag + 1 < args.Length ? args[flag + 1] : null;
        if (image is null)
        {
            string remembered = Path.Combine(dataRoot, "system-image.path");
            try
            {
                if (File.Exists(remembered))
                    image = await File.ReadAllTextAsync(remembered);
            }
            catch (IOException error)
            {
                System.Diagnostics.Trace.WriteLine(error);
            }
            catch (UnauthorizedAccessException error)
            {
                System.Diagnostics.Trace.WriteLine(error);
            }
            if (string.IsNullOrEmpty(image))
                image = RuntimeLayout.SystemImage;
        }
        try
        {
            return File.Exists(image) ? Path.GetFullPath(image) : null;
        }
        catch (ArgumentException)
        {
            return null;
        }
    }

    // The native manager validates the image and package-store compatibility,
    // then atomically remembers it. Never persist an unverified picker result.
    internal static async Task ValidateAsync(PackageManagerClient manager, string dataRoot, string image)
    {
        PackageOperationResult result = await manager.ExecuteAsync("list", null, dataRoot, image);
        if (result.Status != "success")
            throw new IOException(result.Message);
    }
}
