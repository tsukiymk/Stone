using Microsoft.UI.Xaml;
using StoneSettings.Helpers;
using StoneSettings.Services;
using StoneSettings.Utils;
using StoneSettings.Models;

namespace StoneSettings.ViewModels;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class WelcomePageViewModel : ObservableObject
{
    private string dataRoot = "";
    private bool picking;
    private readonly SystemImageDownloadSession downloader = new();
    public SystemImageDownloadState Download => downloader.State;
    internal async Task DownloadImageAsync(Func<SystemImagePackage, Task<bool>> approve)
    {
        if (!CanChooseImage)
            return;
        Error = "";
        NotifyState();
        await downloader.DownloadAsync(approve, SetPicking);
    }
    internal void CancelDownload() => downloader.Cancel();
    public bool IsPreparing
    {
        get; private set;
    }
    public bool CanChooseImage => !picking && !IsPreparing;
    public string Error { get; private set; } = "";
    public bool HasError => Error.Length != 0;
    public Visibility PreparingVisibility => IsPreparing ? Visibility.Visible : Visibility.Collapsed;
    public Visibility ErrorVisibility => Error.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
    internal void Configure(string root) => dataRoot = root;
    internal void SetPicking(bool value)
    {
        picking = value;
        Download.SetAvailable(CanChooseImage);
        Notify(nameof(CanChooseImage));
    }

    internal async Task<bool> UseImageAsync(string image)
    {
        if (IsPreparing)
            return false;
        IsPreparing = true;
        Error = "";
        NotifyState();
        try
        {
            await SystemImageSelection.ValidateAsync(new(RuntimeLayout.StoneExecutable), dataRoot, image);
            return true;
        }
        catch (Exception error)
        {
            ReportError(error);
            return false;
        }
        finally
        {
            IsPreparing = false;
            NotifyState();
        }
    }

    internal void ReportError(Exception error)
    {
        System.Diagnostics.Trace.WriteLine(error);
        Error = UiText.Get("UseImageFailed") + Environment.NewLine + error.Message;
        NotifyState();
    }

    private void NotifyState()
    {
        Download.SetAvailable(CanChooseImage);
        foreach (string property in new[] { nameof(IsPreparing), nameof(CanChooseImage), nameof(Error), nameof(HasError), nameof(PreparingVisibility), nameof(ErrorVisibility) })
            Notify(property);
    }
}
