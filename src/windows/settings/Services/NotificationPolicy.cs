using System.Text;
using System.Text.RegularExpressions;

namespace StoneSettings.Services;

internal sealed partial class NotificationPolicy
{
    public bool Global { get; private set; } = true;
    private readonly Dictionary<string, bool> packages = new(StringComparer.Ordinal);
    public bool Allowed(string package) => packages.GetValueOrDefault(package);
    private static string PathFor(string root) => Path.Combine(root, "data", "system", "stone-notifications", "policy.properties");
    [GeneratedRegex(@"^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$", RegexOptions.CultureInvariant)]
    private static partial Regex PackageName();
    internal static bool IsPackageName(string value) => PackageName().IsMatch(value);

    public static NotificationPolicy Load(string root)
    {
        string path = PathFor(root);
        NotificationPolicy policy = new();
        if (!File.Exists(path))
            return policy;
        using FileStream input = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (input.Length > 1024 * 1024)
            throw new InvalidDataException("Notification policy exceeds its size limit.");
        using StreamReader reader = new(input, Encoding.UTF8, true);
        bool format = false;
        while (reader.ReadLine() is string raw)
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#'))
                continue;
            int split = line.IndexOf('=');
            if (split <= 0)
                throw new InvalidDataException("Invalid notification policy entry.");
            string key = line[..split], value = line[(split + 1)..];
            if (key == "format")
            {
                if (value != "1")
                    throw new InvalidDataException("Unsupported notification policy.");
                format = true;
            }
            else if (key == "global")
                policy.Global = Parse(value);
            else if (key.StartsWith("package.", StringComparison.Ordinal) && PackageName().IsMatch(key[8..]))
                policy.packages[key[8..]] = Parse(value);
            else
                throw new InvalidDataException("Invalid notification policy key.");
        }
        if (!format)
            throw new InvalidDataException("Notification policy has no format.");
        return policy;
    }
    private static bool Parse(string value) => value switch { "true" => true, "false" => false, _ => throw new InvalidDataException("Invalid notification permission.") };
    public static Task<NotificationPolicy> SetAsync(string root, string? package, bool allowed) => Task.Run(() =>
    {
        if (package is not null && !PackageName().IsMatch(package))
            throw new InvalidDataException("Invalid notification package.");
        string path = PathFor(root);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using FileStream gate = new(Path.Combine(Path.GetDirectoryName(path)!, "policy.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        DateTime deadline = DateTime.UtcNow.AddSeconds(5);
        while (true)
        {
            try
            {
                gate.Lock(0, 1);
                break;
            }
            catch (IOException error) when ((error.HResult & 0xffff) == 33 && DateTime.UtcNow < deadline)
            {
                Thread.Sleep(20);
            }
        }
        try
        {
            NotificationPolicy policy = Load(root);
            if (package is null)
                policy.Global = allowed;
            else
                policy.packages[package] = allowed;
            string text = "format=1\nglobal=" + (policy.Global ? "true" : "false") + "\n" + string.Concat(policy.packages.OrderBy(pair => pair.Key, StringComparer.Ordinal)
                .Select(pair => $"package.{pair.Key}={(pair.Value ? "true" : "false")}\n"));
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (FileStream output = new(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    output.Write(Encoding.UTF8.GetBytes(text));
                    output.Flush(true);
                }
                File.Move(temporary, path, true);
            }
            finally
            {
                if (File.Exists(temporary))
                    File.Delete(temporary);
            }
            return policy;
        }
        finally
        {
            gate.Unlock(0, 1);
        }
    });
}
