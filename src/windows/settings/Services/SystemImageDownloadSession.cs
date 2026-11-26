using StoneSettings.Models;

namespace StoneSettings.Services;

internal sealed class SystemImageDownloadSession
{
    private static readonly System.Net.Http.HttpClient client = SystemImageDownload.CreateClient();
    private CancellationTokenSource? pending;
    internal SystemImageDownloadState State { get; } = new();
    internal void Cancel() => pending?.Cancel();

    internal async Task DownloadAsync(Func<SystemImagePackage, Task<bool>> approveLicense, Action<bool> setBusy)
    {
        if (pending is not null)
            return;
        using CancellationTokenSource operation = new();
        pending = operation;
        State.Begin();
        setBusy(true);
        try
        {
            SystemImageDownload downloader = new(client);
            SystemImagePackage package = await downloader.ResolveAsync(operation.Token);
            operation.Token.ThrowIfCancellationRequested();
            if (!await approveLicense(package))
            {
                State.Cancelled();
                return;
            }
            operation.Token.ThrowIfCancellationRequested();
            string image = await downloader.DownloadAsync(package, State.Directory,
                new Progress<ImageDownloadProgress>(value =>
                {
                    if (ReferenceEquals(pending, operation) && !operation.IsCancellationRequested)
                        State.Update(value);
                }), operation.Token);
            State.Complete(image);
        }
        catch (OperationCanceledException) when (operation.IsCancellationRequested)
        {
            State.Cancelled();
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.WriteLine(error);
            State.Failed(error);
        }
        finally
        {
            pending = null;
            State.End();
            setBusy(false);
        }
    }
}
