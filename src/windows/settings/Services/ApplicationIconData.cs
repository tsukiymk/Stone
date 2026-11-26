using System.Buffers.Binary;

namespace StoneSettings.Services;

internal static class ApplicationIconData
{
    internal const int MaximumBytes = 32768;

    internal static byte[] Decode(string? encoded)
    {
        if (string.IsNullOrEmpty(encoded) || encoded.Length > 4 * ((MaximumBytes + 2) / 3))
            return [];
        try
        {
            byte[] bytes = Convert.FromBase64String(encoded);
            return IsPng(bytes) ? bytes : [];
        }
        catch (FormatException) { return []; }
    }

    private static bool IsPng(ReadOnlySpan<byte> bytes) => bytes.Length is >= 33 and <= MaximumBytes
        && bytes[..8].SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 })
        && BinaryPrimitives.ReadUInt32BigEndian(bytes[8..]) == 13
        && bytes.Slice(12, 4).SequenceEqual("IHDR"u8)
        && BinaryPrimitives.ReadUInt32BigEndian(bytes[16..]) is > 0 and <= 256
        && BinaryPrimitives.ReadUInt32BigEndian(bytes[20..]) is > 0 and <= 256;

    internal static async Task<string> LoadAsync(string dataRoot, string packageName)
    {
        // Never accept a path from the package catalog or confirmation caller.
        if (!NotificationPolicy.IsPackageName(packageName))
            return "";
        try
        {
            string path = Path.Combine(dataRoot, "data", "system", "stone-packages", "icons", packageName + ".png");
            await using FileStream input = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete,
                4096, FileOptions.Asynchronous | FileOptions.SequentialScan);
            if (input.Length is < 33 or > MaximumBytes)
                return "";
            byte[] bytes = new byte[(int)input.Length];
            await input.ReadExactlyAsync(bytes);
            return IsPng(bytes) ? Convert.ToBase64String(bytes) : "";
        }
        catch (IOException) { return ""; }
        catch (UnauthorizedAccessException) { return ""; }
    }
}
