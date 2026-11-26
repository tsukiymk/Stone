using System.Globalization;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Xml;
using System.Xml.Linq;

namespace StoneSettings.Services;

internal enum ImageDownloadPhase { Resolving, Downloading, Verifying, Extracting }
internal sealed record ImageDownloadProgress(ImageDownloadPhase Phase, long Completed = 0, long Total = 0);
internal sealed record SystemImagePackage(Uri Archive, long Size, string HashAlgorithm, string Checksum,
    string Revision, string License);

// No ART or privileged Service operations: downloads run inside StoneSettings.
internal sealed class SystemImageDownload(HttpClient client)
{
    internal static readonly Uri Repository = new("https://dl.google.com/android/repository/sys-img/android/sys-img2-3.xml");
    private static readonly Uri ArchiveRoot = new(Repository, ".");
    private const string PackagePath = "system-images;android-37.0;default;x86_64";
    private const long MaximumArchive = 8L * 1024 * 1024 * 1024;
    private const long MaximumImage = 24L * 1024 * 1024 * 1024;
    private const int MaximumCatalog = 8 * 1024 * 1024;

    internal static string DefaultDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "Trebledot Productions", "Stone", "System Images");

    internal static HttpClient CreateClient() => new(new HttpClientHandler { AllowAutoRedirect = false })
    {
        // Individual reads have their own timeout; a complete image can take hours.
        Timeout = Timeout.InfiniteTimeSpan
    };

    internal async Task<SystemImagePackage> ResolveAsync(CancellationToken cancellationToken)
    {
        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(60));
        using var response = await client.GetAsync(Repository, HttpCompletionOption.ResponseHeadersRead, timeout.Token).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength > MaximumCatalog)
            throw new InvalidDataException("The Android image catalog exceeds its size limit.");
        await using Stream source = await response.Content.ReadAsStreamAsync(timeout.Token).ConfigureAwait(false);
        using MemoryStream bytes = new();
        byte[] buffer = new byte[65536];
        int count;
        while ((count = await source.ReadAsync(buffer, timeout.Token).ConfigureAwait(false)) != 0)
        {
            if (bytes.Length + count > MaximumCatalog)
                throw new InvalidDataException("The Android image catalog exceeds its size limit.");
            bytes.Write(buffer, 0, count);
        }
        bytes.Position = 0;
        return ParseRepository(bytes);
    }

    internal static SystemImagePackage ParseRepository(Stream stream)
    {
        using XmlReader reader = XmlReader.Create(stream, new XmlReaderSettings
        {
            DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = MaximumCatalog
        });
        XDocument document = XDocument.Load(reader);
        static XElement? Child(XElement? element, string name) => element?.Elements().FirstOrDefault(value => value.Name.LocalName == name);
        static string Text(XElement? element, string name) => Child(element, name)?.Value.Trim() ?? "";
        static int Number(XElement? element, string name) => int.TryParse(Text(element, name), NumberStyles.None,
            CultureInfo.InvariantCulture, out int value) && value >= 0 ? value : 0;
        var packages = document.Descendants().Where(element => element.Name.LocalName == "remotePackage" &&
            (string?)element.Attribute("path") == PackagePath && Child(element, "obsolete") is null &&
            (string?)Child(element, "channelRef")?.Attribute("ref") == "channel-0" &&
            Text(Child(element, "type-details"), "codename").Length == 0)
            .OrderByDescending(element => Number(Child(element, "revision"), "major"))
            .ThenByDescending(element => Number(Child(element, "revision"), "minor"))
            .ThenByDescending(element => Number(Child(element, "revision"), "micro"));
        XElement package = packages.FirstOrDefault() ?? throw new InvalidDataException("The official catalog has no stable Android API 37.0 x86_64 AOSP image.");
        XElement details = Child(package, "type-details") ?? throw new InvalidDataException("Missing Android image details.");
        if (Text(details, "api-level") != "37.0" || Text(details, "abi") != "x86_64")
            throw new InvalidDataException("The Android image architecture or API does not match its package identity.");
        XElement archive = Child(package, "archives")?.Elements().FirstOrDefault(element => element.Name.LocalName == "archive" &&
            (Text(element, "host-os") is "" or "windows")) ?? throw new InvalidDataException("No Windows-compatible image archive.");
        XElement complete = Child(archive, "complete") ?? throw new InvalidDataException("Missing complete image archive.");
        if (!long.TryParse(Text(complete, "size"), NumberStyles.None, CultureInfo.InvariantCulture, out long size) || size <= 0 || size > MaximumArchive)
            throw new InvalidDataException("Invalid Android image archive size.");
        string relative = Text(complete, "url");
        Uri url = new(ArchiveRoot, relative);
        if (url.Scheme != Uri.UriSchemeHttps || url.Host != ArchiveRoot.Host || url.Port != 443 ||
            url.UserInfo.Length != 0 || url.Query.Length != 0 || url.Fragment.Length != 0 ||
            !url.AbsolutePath.StartsWith(ArchiveRoot.AbsolutePath, StringComparison.Ordinal) ||
            !url.AbsolutePath.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The image archive is outside the official HTTPS repository.");
        XElement checksum = Child(complete, "checksum") ?? throw new InvalidDataException("Missing image checksum.");
        string algorithm = ((string?)checksum.Attribute("type") ?? "sha1").ToLowerInvariant();
        int hashLength = algorithm switch { "sha1" => 40, "sha256" => 64, "sha512" => 128, _ => 0 };
        string hash = checksum.Value.Trim().ToLowerInvariant();
        if (hashLength == 0 || hash.Length != hashLength || !hash.All(Uri.IsHexDigit))
            throw new InvalidDataException("Invalid Android image checksum.");
        string licenseId = (string?)Child(package, "uses-license")?.Attribute("ref") ?? "";
        string license = document.Descendants().FirstOrDefault(element => element.Name.LocalName == "license" &&
            (string?)element.Attribute("id") == licenseId)?.Value.Trim() ?? "";
        if (licenseId.Length == 0 || license.Length == 0)
            throw new InvalidDataException("Missing Android SDK license.");
        XElement? revision = Child(package, "revision");
        string version = $"{Number(revision, "major")}.{Number(revision, "minor")}.{Number(revision, "micro")}";
        return new(url, size, algorithm, hash, version, license);
    }

    internal async Task<string> DownloadAsync(SystemImagePackage package, string directory,
        IProgress<ImageDownloadProgress>? progress, CancellationToken cancellationToken)
    {
        directory = Path.GetFullPath(directory);
        Directory.CreateDirectory(directory);
        // Serialize publication across settings windows and processes.
        await using FileStream lease = new(Path.Combine(directory, ".android-37-x86_64.lock"), FileMode.OpenOrCreate,
            FileAccess.ReadWrite, FileShare.None);
        string destination = Path.Combine(directory, "android-37.0-x86_64-r" + package.Revision);
        if (Directory.Exists(destination))
            return await ValidateExistingAsync(destination, package, progress, cancellationToken).ConfigureAwait(false);
        if (new DriveInfo(Path.GetPathRoot(directory)!).AvailableFreeSpace < package.Size)
            throw new IOException("There is not enough free space to download the Android system image.");
        string staging = Path.Combine(directory, ".partial-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(staging);
        string archivePath = Path.Combine(staging, "image.zip");
        string imagePath = Path.Combine(staging, "system.img");
        try
        {
            await DownloadArchiveAsync(package, archivePath, progress, cancellationToken).ConfigureAwait(false);
            string imageHash = await ExtractImageAsync(archivePath, imagePath, progress, cancellationToken).ConfigureAwait(false);
            await File.WriteAllLinesAsync(Path.Combine(staging, "download.receipt"),
                new[] { package.Archive.AbsoluteUri, package.Checksum, imageHash }, cancellationToken).ConfigureAwait(false);
            await File.WriteAllTextAsync(Path.Combine(staging, "LICENSE.txt"), package.License, cancellationToken).ConfigureAwait(false);
            File.Delete(archivePath);
            cancellationToken.ThrowIfCancellationRequested();
            Directory.Move(staging, destination);
            return Path.Combine(destination, "system.img");
        }
        finally
        {
            // Only these explicit files are ever extracted; do not recurse over
            // archive paths or remove an already published/selected image.
            if (Directory.Exists(staging))
            {
                try
                {
                    foreach (string file in new[] { "image.zip", "system.img", "download.receipt", "LICENSE.txt" })
                        File.Delete(Path.Combine(staging, file));
                    Directory.Delete(staging);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    System.Diagnostics.Trace.WriteLine(error);
                }
            }
        }
    }

    private async Task DownloadArchiveAsync(SystemImagePackage package, string path, IProgress<ImageDownloadProgress>? progress,
        CancellationToken cancellationToken)
    {
        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(60));
        using var response = await client.GetAsync(package.Archive, HttpCompletionOption.ResponseHeadersRead, timeout.Token).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is long length && length != package.Size)
            throw new InvalidDataException("The Android image archive size differs from the official catalog.");
        await using Stream source = await response.Content.ReadAsStreamAsync(timeout.Token).ConfigureAwait(false);
        await using FileStream output = new(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, true);
        using IncrementalHash hash = IncrementalHash.CreateHash(new HashAlgorithmName(package.HashAlgorithm.ToUpperInvariant()));
        byte[] buffer = new byte[131072];
        long received = 0, lastUpdate = Environment.TickCount64 - 250;
        progress?.Report(new(ImageDownloadPhase.Downloading, 0, package.Size));
        while (true)
        {
            timeout.CancelAfter(TimeSpan.FromSeconds(60));
            int count = await source.ReadAsync(buffer, timeout.Token).ConfigureAwait(false);
            if (count == 0)
                break;
            received += count;
            if (received > package.Size)
                throw new InvalidDataException("The Android image archive exceeds its catalog size.");
            await output.WriteAsync(buffer.AsMemory(0, count), cancellationToken).ConfigureAwait(false);
            hash.AppendData(buffer, 0, count);
            if (Environment.TickCount64 - lastUpdate >= 200)
            {
                progress?.Report(new(ImageDownloadPhase.Downloading, received, package.Size));
                lastUpdate = Environment.TickCount64;
            }
        }
        progress?.Report(new(ImageDownloadPhase.Verifying));
        if (received != package.Size || !CryptographicOperations.FixedTimeEquals(hash.GetHashAndReset(), Convert.FromHexString(package.Checksum)))
            throw new InvalidDataException("The downloaded Android image failed its catalog checksum or length verification.");
        await output.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    private static async Task<string> ExtractImageAsync(string archivePath, string destination,
        IProgress<ImageDownloadProgress>? progress, CancellationToken cancellationToken)
    {
        using ZipArchive archive = ZipFile.OpenRead(archivePath);
        var images = archive.Entries.Where(entry => entry.FullName.Replace('\\', '/') is "x86_64/system.img" or "system.img").ToArray();
        if (images.Length != 1 || images[0].Length < 4096 || images[0].Length > MaximumImage ||
            ((images[0].ExternalAttributes >> 16) & 0xf000) == 0xa000)
            throw new InvalidDataException("The archive does not contain one complete, regular x86_64 system.img.");
        ZipArchiveEntry image = images[0];
        DriveInfo drive = new(Path.GetPathRoot(destination)!);
        if (drive.AvailableFreeSpace < image.Length)
            throw new IOException("There is not enough free space to extract the Android system image.");
        await using Stream source = image.Open();
        await using FileStream output = new(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, true);
        using IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buffer = new byte[131072];
        long received = 0, lastUpdate = Environment.TickCount64 - 250;
        int count;
        while ((count = await source.ReadAsync(buffer, cancellationToken).ConfigureAwait(false)) != 0)
        {
            received += count;
            if (received > image.Length)
                throw new InvalidDataException("The extracted system image exceeds its declared length.");
            await output.WriteAsync(buffer.AsMemory(0, count), cancellationToken).ConfigureAwait(false);
            hash.AppendData(buffer, 0, count);
            if (Environment.TickCount64 - lastUpdate >= 200)
            {
                progress?.Report(new(ImageDownloadPhase.Extracting, received, image.Length));
                lastUpdate = Environment.TickCount64;
            }
        }
        if (received != image.Length)
            throw new InvalidDataException("The extracted system image is truncated.");
        await output.FlushAsync(cancellationToken).ConfigureAwait(false);
        return Convert.ToHexStringLower(hash.GetHashAndReset());
    }

    private static async Task<string> ValidateExistingAsync(string directory, SystemImagePackage package,
        IProgress<ImageDownloadProgress>? progress, CancellationToken cancellationToken)
    {
        string receiptPath = Path.Combine(directory, "download.receipt");
        string imagePath = Path.Combine(directory, "system.img");
        if (!File.Exists(receiptPath) || new FileInfo(receiptPath).Length > 4096 || !File.Exists(imagePath))
            throw new InvalidDataException("An incomplete image already exists at " + directory);
        string[] receipt = await File.ReadAllLinesAsync(receiptPath, cancellationToken).ConfigureAwait(false);
        if (receipt.Length != 3 || receipt[0] != package.Archive.AbsoluteUri || receipt[1] != package.Checksum ||
            receipt[2].Length != 64 || !receipt[2].All(Uri.IsHexDigit))
            throw new InvalidDataException("The existing image does not match the official catalog: " + directory);
        progress?.Report(new(ImageDownloadPhase.Verifying));
        await using FileStream image = new(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read, 131072, true);
        if (image.Length < 4096 || image.Length > MaximumImage ||
            !CryptographicOperations.FixedTimeEquals(await SHA256.HashDataAsync(image, cancellationToken).ConfigureAwait(false), Convert.FromHexString(receipt[2])))
            throw new InvalidDataException("The existing system image was modified: " + imagePath);
        return imagePath;
    }
}
