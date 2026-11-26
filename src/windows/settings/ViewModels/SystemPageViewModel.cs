using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;

namespace StoneSettings.ViewModels;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class SystemPageViewModel : ObservableObject
{
    private bool busy;
    private string imagePath = UiText.Get("NoImage");
    public bool IsIdle => !busy;
    internal SystemImageDownloadSession Downloader { get; } = new();
    public SystemImageDownloadState Download => Downloader.State;
    internal Func<Task>? DownloadImageRequested { get; set; }
    internal Func<Task>? UseDownloadedImageRequested { get; set; }
    internal Task DownloadImageAsync() => DownloadImageRequested?.Invoke() ?? throw new InvalidOperationException("The settings page has not been configured.");
    internal Task UseDownloadedImageAsync() => UseDownloadedImageRequested?.Invoke() ?? throw new InvalidOperationException("The settings page has not been configured.");
    internal void CancelDownload() => Downloader.Cancel();
    public string ImagePath
    {
        get => imagePath;
        internal set => Set(ref imagePath, value);
    }
    internal Func<Task>? SelectImageRequested
    {
        get; set;
    }
    internal Task SelectImageAsync() => SelectImageRequested?.Invoke() ?? throw new InvalidOperationException("The settings page has not been configured.");
    internal void SetBusy(bool value)
    {
        busy = value;
        Download.SetAvailable(!value);
        Notify(nameof(IsIdle));
    }
}
