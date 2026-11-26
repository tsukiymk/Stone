using System.IO.Compression;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using StoneSettings.Services;

static class Tests
{
    static readonly byte[] image = Enumerable.Range(0, 8192).Select(value => (byte)(value % 251)).ToArray();
    static void Require(bool value, string message) { if (!value) throw new Exception(message); }
    static async Task Reject<T>(Func<Task> work) where T : Exception
    {
        try { await work(); }
        catch (T) { return; }
        throw new Exception("Expected " + typeof(T).Name);
    }
    static byte[] Archive(bool duplicate = false, bool missing = false)
    {
        using MemoryStream bytes = new();
        using (ZipArchive zip = new(bytes, ZipArchiveMode.Create, true))
        {
            if (!missing)
                using (Stream stream = zip.CreateEntry("x86_64/system.img").Open()) stream.Write(image);
            if (duplicate)
                using (Stream stream = zip.CreateEntry("system.img").Open()) stream.Write(image);
            // Extract only the image to a fixed path; never traverse ZIP names.
            using (Stream stream = zip.CreateEntry("../../escaped.txt").Open()) stream.WriteByte(1);
        }
        return bytes.ToArray();
    }
    static string Catalog(byte[] archive, string? checksum = null, string url = "x86_64-37.0_r06.zip") => $$"""
        <repository>
          <license id="android-sdk-license">Test SDK license.</license>
          <remotePackage path="system-images;android-37.0;default;x86_64">
            <type-details><api-level>37.0</api-level><abi>x86_64</abi></type-details>
            <revision><major>6</major></revision><channelRef ref="channel-0"/>
            <uses-license ref="android-sdk-license"/>
            <archives><archive><complete><size>{{archive.Length}}</size>
              <checksum type="sha1">{{checksum ?? Convert.ToHexStringLower(SHA1.HashData(archive))}}</checksum>
              <url>{{url}}</url></complete></archive></archives>
          </remotePackage>
        </repository>
        """;

    sealed class Server(string catalog, byte[] archive) : HttpMessageHandler
    {
        internal int Archives;
        internal bool FailArchive;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (request.RequestUri == SystemImageDownload.Repository)
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(catalog) });
            Require(request.RequestUri?.Host == "dl.google.com", "Downloader contacted an unexpected host");
            Archives++;
            return Task.FromResult(new HttpResponseMessage(FailArchive ? HttpStatusCode.NotFound : HttpStatusCode.OK)
            { Content = new ByteArrayContent(archive) });
        }
    }
    sealed class InlineProgress(Action<ImageDownloadProgress> report) : IProgress<ImageDownloadProgress>
    {
        public void Report(ImageDownloadProgress value) => report(value);
    }
    static void Clean(string directory)
    {
        Require(!Directory.Exists(directory) || !Directory.EnumerateDirectories(directory, ".partial-*").Any(),
            "Failed or cancelled download left a staging directory");
    }

    static async Task Pipeline(string root)
    {
        byte[] zip = Archive();
        using Server server = new(Catalog(zip), zip);
        using HttpClient client = new(server);
        SystemImageDownload downloader = new(client);
        SystemImagePackage package = await downloader.ResolveAsync(CancellationToken.None);
        Require(package.Revision == "6.0.0" && package.License == "Test SDK license.", "Wrong package or license selected");
        List<ImageDownloadPhase> phases = [];
        string destination = await downloader.DownloadAsync(package, root,
            new InlineProgress(value => phases.Add(value.Phase)), CancellationToken.None);
        Require((await File.ReadAllBytesAsync(destination)).SequenceEqual(image), "Extracted image bytes changed");
        Require(destination == Path.Combine(root, "android-37.0-x86_64-r6.0.0", "system.img"), "Unexpected image destination");
        Require(File.Exists(Path.Combine(Path.GetDirectoryName(destination)!, "LICENSE.txt")), "SDK license was not retained");
        Require(!File.Exists(Path.Combine(root, "escaped.txt")) && !File.Exists(Path.Combine(Path.GetDirectoryName(root)!, "escaped.txt")),
            "ZIP paths escaped the image directory");
        Require(phases.Contains(ImageDownloadPhase.Downloading) && phases.Contains(ImageDownloadPhase.Verifying) &&
            phases.Contains(ImageDownloadPhase.Extracting), "Download stages were not reported");
        string reused = await downloader.DownloadAsync(package, root, null, CancellationToken.None);
        Require(reused == destination && server.Archives == 1, "Verified existing image was unnecessarily redownloaded");
        await File.WriteAllBytesAsync(destination, new byte[8192]);
        await Reject<InvalidDataException>(() => downloader.DownloadAsync(package, root, null, CancellationToken.None));
        Require(server.Archives == 1, "Modified image was silently replaced");
        Clean(root);
    }

    static async Task Failure(string root, byte[] zip, string? checksum = null, bool cancelExtract = false, bool networkFailure = false)
    {
        using Server server = new(Catalog(zip, checksum), zip) { FailArchive = networkFailure };
        using HttpClient client = new(server);
        SystemImageDownload downloader = new(client);
        SystemImagePackage package = await downloader.ResolveAsync(CancellationToken.None);
        using CancellationTokenSource cancellation = new();
        IProgress<ImageDownloadProgress>? progress = cancelExtract ? new InlineProgress(value =>
        {
            if (value.Phase == ImageDownloadPhase.Extracting) cancellation.Cancel();
        }) : null;
        if (cancelExtract)
            await Reject<OperationCanceledException>(() => downloader.DownloadAsync(package, root, progress, cancellation.Token));
        else if (networkFailure)
            await Reject<HttpRequestException>(() => downloader.DownloadAsync(package, root, progress, cancellation.Token));
        else
            await Reject<InvalidDataException>(() => downloader.DownloadAsync(package, root, progress, cancellation.Token));
        Clean(root);
        Require(!Directory.EnumerateDirectories(root, "android-*").Any(), "Failed image was published");
    }

    internal static async Task<int> Main(string[] arguments)
    {
        try
        {
            if (arguments.Length == 2 && arguments[0] == "--catalog")
            {
                using FileStream catalog = File.OpenRead(arguments[1]);
                SystemImagePackage package = SystemImageDownload.ParseRepository(catalog);
                Console.WriteLine($"PASS live catalog: {package.Archive} revision={package.Revision} bytes={package.Size} {package.HashAlgorithm}={package.Checksum}");
                return 0;
            }
            Require(arguments.Length == 1, "Missing isolated test directory");
            string root = Path.Combine(Path.GetFullPath(arguments[0]), Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            byte[] zip = Archive();
            foreach (string invalid in new[] {
                Catalog(zip, url: "https://example.com/system.zip"),
                Catalog(zip).Replace("<api-level>37.0</api-level>", "<api-level>36</api-level>"),
                Catalog(zip).Replace("channel-0", "channel-1"),
                Catalog(zip).Replace("android-sdk-license\"/>", "missing-license\"/>"),
                Catalog(zip, checksum: "invalid") })
            {
                await Reject<InvalidDataException>(() => Task.Run(() =>
                    SystemImageDownload.ParseRepository(new MemoryStream(Encoding.UTF8.GetBytes(invalid)))));
            }
            await Reject<System.Xml.XmlException>(() => Task.Run(() => SystemImageDownload.ParseRepository(
                new MemoryStream(Encoding.UTF8.GetBytes("<!DOCTYPE repository [<!ENTITY x SYSTEM 'file:///not-used'>]><repository>&x;</repository>")))));
            await Pipeline(Path.Combine(root, "complete"));
            await Failure(Path.Combine(root, "checksum"), zip, new string('0', 40));
            await Failure(Path.Combine(root, "duplicate"), Archive(duplicate: true));
            await Failure(Path.Combine(root, "missing"), Archive(missing: true));
            await Failure(Path.Combine(root, "cancel"), zip, cancelExtract: true);
            await Failure(Path.Combine(root, "network"), zip, networkFailure: true);
            Console.WriteLine("PASS official catalog policy, download/extraction bytes, progress, SDK license, hash verification, cached-image tampering, cancellation, ZIP boundaries and failure cleanup");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
