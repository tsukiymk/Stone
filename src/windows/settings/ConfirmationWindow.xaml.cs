using System.Buffers.Binary;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Win32.SafeHandles;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Utils;
using StoneSettings.Views;

namespace StoneSettings;

public sealed partial class ConfirmationWindow : Window
{
    private readonly string[] arguments;
    private readonly CancellationTokenSource lifetime = new(TimeSpan.FromMinutes(5));
    private bool started;
    private readonly Microsoft.UI.Windowing.DisplayArea confirmationDisplay = PermissionDialog.GetTargetDisplay();

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint processId);

    public ConfirmationWindow(string[] arguments)
    {
        this.arguments = arguments;
        InitializeComponent();
        Title = UiText.Get("SettingsTitle");
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(WindowTitleBar);
        WindowSizing.Resize(this, 660, 460);
        Closed += (_, _) => lifetime.Cancel();
        Root.Loaded += async (_, _) =>
        {
            if (!started)
            {
                started = true;
                await RunAsync();
            }
        };
    }

    private string Argument(string name)
    {
        int index = Array.IndexOf(arguments, name);
        if (index < 0 || index + 1 == arguments.Length)
            throw new InvalidDataException("Missing confirmation argument.");
        return arguments[index + 1];
    }
    private async Task RunAsync()
    {
        try
        {
            string name = Argument("--confirmation-pipe");
            uint parent = uint.Parse(Argument("--confirmation-parent"), System.Globalization.CultureInfo.InvariantCulture);
            string prefix = $"Stone.Settings.{parent}.";
            if (!name.StartsWith(prefix, StringComparison.Ordinal) || !Guid.TryParseExact(name[prefix.Length..], "D", out Guid id))
                throw new InvalidDataException("Invalid confirmation pipe.");
            await using NamedPipeClientStream pipe = new(".", name, PipeDirection.InOut,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            await pipe.ConnectAsync(15000, lifetime.Token);
            if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle, out uint server) || server != parent)
                throw new InvalidDataException("Unexpected confirmation host.");
            byte[] length = new byte[4];
            await pipe.ReadExactlyAsync(length, lifetime.Token);
            uint count = BinaryPrimitives.ReadUInt32LittleEndian(length);
            if (count is 0 or > 65536)
                throw new InvalidDataException("Invalid confirmation frame.");
            byte[] payload = new byte[count];
            await pipe.ReadExactlyAsync(payload, lifetime.Token);
            ConfirmationEnvelope envelope = JsonSerializer.Deserialize(payload, SettingsJson.Default.ConfirmationEnvelope)
                ?? throw new InvalidDataException("Missing confirmation request.");
            PackageConfirmationData request = envelope.Confirmation;
            if (envelope.Format != 1 || envelope.ParentProcessId != parent
                || !Guid.TryParseExact(envelope.RequestId, "D", out Guid requestId) || requestId != id
                || request is null || string.IsNullOrWhiteSpace(request.PackageName) || request.PackageName.Length > 255
                || string.IsNullOrWhiteSpace(request.Name) || request.Name.Length > 4096 || request.Installer.Length > 4096
                || request.Version.Length > 256 || request.PreviousVersion.Length > 256
                || !string.IsNullOrEmpty(request.PreviousVersion) && request.Operation != "install")
                throw new InvalidDataException("Invalid confirmation identity.");
            PackageConfirmationPage page = new(request);
            Title = request.Operation == "notifications" ? UiText.Get("NotificationTitle") : UiText.Get("InstallerTitle");
            if (request.Operation == "notifications")
            {
                SystemBackdrop = null;
                WindowTitleBar.Visibility = Visibility.Collapsed;
                Root.RowDefinitions[0].Height = new GridLength(0);
                PermissionDialog.Configure(this, confirmationDisplay);
            }
            // This read also observes host cancellation/death while the user is
            // deciding. The only normal incoming byte is the final acknowledgment.
            byte[] acknowledgment = new byte[1];
            Task incoming = pipe.ReadExactlyAsync(acknowledgment, lifetime.Token).AsTask();
            PageHost.Content = page;
            Task<ContentDialogResult> decision = page.Decision;
            if (await Task.WhenAny(decision, incoming) != decision)
            {
                await incoming;
                throw new InvalidDataException("Confirmation host responded before a decision.");
            }
            byte result = await decision == ContentDialogResult.Primary ? (byte)1 : (byte)0;
            await pipe.WriteAsync(new[] { result }, lifetime.Token);
            await incoming;
            if (acknowledgment[0] != 0xac)
                throw new InvalidDataException("Invalid confirmation acknowledgment.");
        }
        catch (Exception error)
        {
            System.Diagnostics.Debug.WriteLine(error);
        }
        finally
        {
            Application.Current.Exit();
        }
    }
}
