using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using StoneSettings.Utils;

namespace StoneSettings.Services;

internal static partial class NotificationActivation
{
    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint pid);

    internal static async Task ActivateAsync(string value, string? diagnosticServer = null)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out Uri? uri) || !uri.Scheme.StartsWith("stone-notify-", StringComparison.Ordinal)
                || uri.Scheme.Length != 29 || uri.Scheme[13..].Any(ch => !Uri.IsHexDigit(ch))
                || uri.UserInfo.Length != 0 || uri.Port != -1 || uri.Query.Length != 0 || uri.Fragment.Length != 0)
            throw new InvalidDataException("Invalid notification activation identity.");
        string[] path = uri.AbsolutePath.Split('/');
        if (uri.Host == "service")
        {
            if (diagnosticServer is not null || path.Length != 3 || path[1].Length != 32
                    || path[1].Any(ch => ch is not (>= '0' and <= '9') and not (>= 'a' and <= 'f'))
                    || !int.TryParse(path[2], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out int routedAction) || routedAction is < -1 or > 4)
                throw new InvalidDataException("Invalid service notification route.");
            ProcessStartInfo start = new(Path.GetFullPath(RuntimeLayout.ServiceExecutable))
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            start.ArgumentList.Add("--notification-activate");
            start.ArgumentList.Add(path[1]);
            start.ArgumentList.Add(routedAction.ToString(CultureInfo.InvariantCulture));
            using Process request = Process.Start(start) ?? throw new IOException("Cannot start notification service request.");
            Task<string> output = request.StandardOutput.ReadToEndAsync(), errors = request.StandardError.ReadToEndAsync();
            using CancellationTokenSource deadline = new(TimeSpan.FromSeconds(15));
            try
            {
                await request.WaitForExitAsync(deadline.Token);
            }
            catch (OperationCanceledException)
            {
                if (!request.HasExited)
                    request.Kill();
                throw new IOException("Notification service request timed out.");
            }
            await Task.WhenAll(output, errors);
            if (request.ExitCode != 0)
                throw new InvalidDataException("Notification is no longer active or allowed.");
            return; // Accepted by Service; execution is performed asynchronously in ART.
        }
        if (!uint.TryParse(uri.Host, NumberStyles.None, CultureInfo.InvariantCulture, out uint parent) || parent is 0 or > int.MaxValue)
            throw new InvalidDataException("Invalid notification process.");
        if (path.Length != 4 || !Guid.TryParseExact(path[1], "D", out Guid session)
                || !ulong.TryParse(path[2], NumberStyles.None, CultureInfo.InvariantCulture, out ulong ticket) || ticket is 0 or > long.MaxValue
                || !int.TryParse(path[3], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out int action) || action is < -1 or > 4)
            throw new InvalidDataException("Invalid notification activation request.");
        using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(8));
        await using NamedPipeClientStream pipe = new(".", $"Stone.Notification.{parent}.{session:D}", PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        await pipe.ConnectAsync(5000, timeout.Token);
        if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle, out uint server) || server != parent)
            throw new InvalidDataException("Unexpected notification process.");
        using Process process = Process.GetProcessById((int)parent);
        string executable = process.MainModule?.FileName ?? throw new InvalidDataException("Notification process has no executable.");
        string expected = Path.GetFullPath(diagnosticServer ?? RuntimeLayout.StoneExecutable);
        bool belongsToRuntime = string.Equals(Path.GetFullPath(executable), expected, StringComparison.OrdinalIgnoreCase)
            || diagnosticServer is null && string.Equals(Path.GetFullPath(executable),
                Path.GetFullPath(RuntimeLayout.ServiceExecutable), StringComparison.OrdinalIgnoreCase);
        if (!belongsToRuntime
                || process.SessionId != Process.GetCurrentProcess().SessionId)
            throw new InvalidDataException("Notification process does not belong to this Stone runtime.");
        byte[] packet = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(packet, 0x31304e53);
        BinaryPrimitives.WriteUInt64LittleEndian(packet.AsSpan(4), ticket);
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(12), action);
        await pipe.WriteAsync(packet, timeout.Token);
        byte[] acknowledgment = new byte[1];
        await pipe.ReadExactlyAsync(acknowledgment, timeout.Token);
        await pipe.WriteAsync(new byte[] { 0xac }, timeout.Token);
        if (acknowledgment[0] != 0xac)
            throw new InvalidDataException("Notification is no longer active.");
    }
}
