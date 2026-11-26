using System.Buffers.Binary;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.Win32.SafeHandles;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;
using StoneSettings.Utils;
using StoneSettings.Views;

namespace StoneSettings;

// One window from file selection through verification, decision and commit.
public sealed partial class InstallWindow : Window
{
    private readonly CancellationTokenSource lifetime = new();
    private readonly CancellationTokenSource preparation = new();
    private readonly TaskCompletionSource loaded = new();
    private readonly TaskCompletionSource closed = new();
    private readonly string file;
    private PackageConfirmationPage page;
    private bool committing, approvalStarted, isClosed;

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeClientProcessId(SafePipeHandle pipe, out uint processId);

    internal InstallWindow(string file)
    {
        this.file = file;
        InitializeComponent();
        Title = UiText.Get("InstallerTitle");
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(WindowTitleBar);
        page = Preparing(UiText.Get("ReadingApk"));
        SetPage(page);
        WindowSizing.Resize(this, 664, 448);
        Root.Loaded += (_, _) => loaded.TrySetResult();
        AppWindow.Closing += (_, args) =>
        {
            if (committing)
                args.Cancel = true;
        };
        Closed += (_, _) =>
        {
            isClosed = true;
            if (!approvalStarted)
                preparation.Cancel();
            lifetime.Cancel();
            closed.TrySetResult();
        };
    }

    private PackageConfirmationPage Preparing(string message) => new(new()
    {
        Operation = "install",
        Name = Path.GetFileName(file),
        Installer = UiText.Get("SettingsTitle")
    }, message);

    private PackageConfirmationPage Failed(PackageOperationResult failure) => new(new()
    {
        Operation = "install",
        Name = Path.GetFileName(file),
        Installer = UiText.Get("SettingsTitle")
    }, failure: failure);

    private void SetPage(PackageConfirmationPage next)
    {
        page = next;
        PageHost.Content = page;
    }

    internal async Task<PackageOperationResult> RunAsync(PackageManagerClient manager, string dataRoot, string? image)
    {
        Task<PackageOperationResult>? operation = null;
        Task? confirmation = null;
        NamedPipeServerStream? pipe = null;
        PackageOperationResult? operationResult = null;
        try
        {
            _ = CloseOnDecisionAsync(page);
            Activate();
            await loaded.Task.WaitAsync(lifetime.Token);
            TaskCompletionSource rendered = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int frames = 0;
            void Rendered(object? sender, object args)
            {
                if (++frames >= 2)
                    rendered.TrySetResult();
            }
            CompositionTarget.Rendering += Rendered;
            try
            {
                await rendered.Task.WaitAsync(lifetime.Token);
            }
            finally
            {
                CompositionTarget.Rendering -= Rendered;
            }
            lifetime.Token.ThrowIfCancellationRequested();
            Guid id = Guid.NewGuid();
            string name = $"Stone.Install.{Environment.ProcessId}.{id:D}";
            using WindowsIdentity identity = WindowsIdentity.GetCurrent();
            SecurityIdentifier user = identity.User ?? throw new IOException(UiText.Get("InstallUserUnavailable"));
            PipeSecurity security = new();
            security.SetAccessRuleProtection(true, false);
            security.AddAccessRule(new(new SecurityIdentifier(WellKnownSidType.NetworkSid, null), PipeAccessRights.FullControl, AccessControlType.Deny));
            security.AddAccessRule(new(user, PipeAccessRights.FullControl, AccessControlType.Allow));
            pipe = NamedPipeServerStreamAcl.Create(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous, 65536, 65536, security, HandleInheritability.None, 0);
            TaskCompletionSource<int> worker = new();
            confirmation = ConfirmAsync(pipe, id, worker.Task);
            operation = manager.ExecuteAsync("install", file, dataRoot, image, name, pid => worker.TrySetResult(pid), preparation.Token);
            if (await Task.WhenAny(operation, confirmation) == confirmation)
            {
                try
                {
                    await confirmation;
                }
                catch (Exception error) when (approvalStarted)
                {
                    System.Diagnostics.Trace.WriteLine(error);
                }
            }
            PackageOperationResult result = await operation;
            operationResult = result;
            if (result.Status == "success")
            {
                if (!approvalStarted)
                    throw new InvalidDataException("Installation completed without the verified decision.");
                committing = false;
                Close();
                await closed.Task;
                return result;
            }
            if (result.Status == "cancelled")
            {
                committing = false;
                Close();
                await closed.Task;
                return result;
            }
            throw new IOException(result.Message);
        }
        catch (OperationCanceledException) when (!committing && lifetime.IsCancellationRequested)
        {
            return new()
            {
                Format = 1,
                Status = "cancelled"
            };
        }
        catch (Exception error)
        {
            if (!approvalStarted)
                preparation.Cancel();
            committing = false;
            if (!isClosed)
            {
                SetPage(Failed(operationResult is { Status: "failed" } ? operationResult
                    : new() { Status = "failed", Message = error.Message }));
                _ = CloseOnDecisionAsync(page);
                await closed.Task;
            }
            return new()
            {
                Format = 1,
                Status = "failed",
                Message = error.Message,
                Compatibility = operationResult?.Compatibility
            };
        }
        finally
        {
            if (!approvalStarted)
                preparation.Cancel();
            lifetime.Cancel();
            if (operation is not null)
                try
                {
                    await operation;
                }
                catch (Exception error)
                {
                    System.Diagnostics.Trace.WriteLine(error);
                }
            if (confirmation is not null)
                try
                {
                    await confirmation;
                }
                catch (Exception error)
                {
                    System.Diagnostics.Trace.WriteLine(error);
                }
            pipe?.Dispose();
            lifetime.Dispose();
            preparation.Dispose();
        }
    }

    private async Task CloseOnDecisionAsync(PackageConfirmationPage current)
    {
        await current.Decision;
        if (!isClosed)
            Close();
    }

    private async Task ConfirmAsync(NamedPipeServerStream pipe, Guid id, Task<int> worker)
    {
        // Before validation there is no decision button that can authorize a commit.
        await pipe.WaitForConnectionAsync(lifetime.Token);
        int expected = await worker.WaitAsync(lifetime.Token);
        if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle, out uint actual) || actual != expected)
            throw new InvalidDataException("Unexpected package-management process.");
        byte[] length = new byte[4];
        await pipe.ReadExactlyAsync(length, lifetime.Token);
        uint count = BinaryPrimitives.ReadUInt32LittleEndian(length);
        if (count is 0 or > 65536)
            throw new InvalidDataException("Invalid installation confirmation frame.");
        byte[] payload = new byte[count];
        await pipe.ReadExactlyAsync(payload, lifetime.Token);
        ConfirmationEnvelope envelope = JsonSerializer.Deserialize(payload, SettingsJson.Default.ConfirmationEnvelope)
            ?? throw new InvalidDataException("Missing installation confirmation.");
        PackageConfirmationData request = envelope.Confirmation;
        if (envelope.Format != 1 || envelope.ParentProcessId != actual || !Guid.TryParseExact(envelope.RequestId, "D", out Guid requestId) || requestId != id
            || request is null || request.Operation != "install" || !request.Verified
            || string.IsNullOrWhiteSpace(request.PackageName) || request.PackageName.Length > 255
            || string.IsNullOrWhiteSpace(request.Name) || request.Name.Length > 4096
            || request.Version is null || request.Version.Length > 256 || request.PreviousVersion is null || request.PreviousVersion.Length > 256
            || request.Installer is null || request.Installer.Length > 4096)
            throw new InvalidDataException("Invalid verified installation identity.");
        lifetime.Token.ThrowIfCancellationRequested();
        SetPage(new(request));
        if (await page.Decision.WaitAsync(lifetime.Token) != ContentDialogResult.Primary)
        {
            Close();
            lifetime.Token.ThrowIfCancellationRequested();
        }
        // Serialize approval and closing on the UI thread. Once a positive byte
        // can reach the worker, neither closing nor cancellation kills it.
        lifetime.Token.ThrowIfCancellationRequested();
        committing = approvalStarted = true;
        page.PrimaryButton.IsEnabled = page.CancelButton.IsEnabled = false;
        page.MessageText.Text = UiText.Get("Installing");
        page.MessageText.Visibility = Visibility.Visible;
        await pipe.WriteAsync(new byte[] { 1 }, lifetime.Token);
        byte[] ack = new byte[1];
        await pipe.ReadExactlyAsync(ack, lifetime.Token);
        if (ack[0] != 0xac)
            throw new InvalidDataException("Invalid installation acknowledgment.");
    }
}
