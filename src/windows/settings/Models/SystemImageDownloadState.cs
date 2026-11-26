using Microsoft.UI.Xaml;
using StoneSettings.Helpers;
using StoneSettings.Services;

namespace StoneSettings.Models;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class SystemImageDownloadState : ObservableObject
{
    private bool available = true;
    public string Directory => SystemImageDownload.DefaultDirectory;
    public bool IsBusy { get; private set; }
    public bool CanDownload => available && !IsBusy;
    public bool CanUse => available && !IsBusy && ImagePath.Length != 0;
    public string ImagePath { get; private set; } = "";
    public string Status { get; private set; } = "";
    public double Percent { get; private set; }
    public bool IsIndeterminate { get; private set; } = true;
    public Visibility ProgressVisibility => IsBusy ? Visibility.Visible : Visibility.Collapsed;
    public Visibility UseVisibility => ImagePath.Length != 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility StatusVisibility => Status.Length != 0 ? Visibility.Visible : Visibility.Collapsed;

    internal void SetAvailable(bool value) { available = value; NotifyState(); }
    internal void Begin()
    {
        IsBusy = true;
        ImagePath = "";
        Update(new(ImageDownloadPhase.Resolving));
    }
    internal void Update(ImageDownloadProgress progress)
    {
        IsIndeterminate = progress.Total <= 0;
        Percent = progress.Total > 0 ? 100.0 * progress.Completed / progress.Total : 0;
        Status = progress.Phase switch
        {
            ImageDownloadPhase.Resolving => UiText.Get("ImageDownloadResolving"),
            ImageDownloadPhase.Verifying => UiText.Get("ImageDownloadVerifying"),
            ImageDownloadPhase.Extracting => UiText.Format("ImageDownloadExtracting", progress.Completed / 1048576.0, progress.Total / 1048576.0),
            _ => UiText.Format("ImageDownloadTransferring", progress.Completed / 1048576.0, progress.Total / 1048576.0)
        };
        NotifyState();
    }
    internal void Complete(string image)
    {
        ImagePath = image;
        Status = UiText.Get("ImageDownloadReady");
        NotifyState();
    }
    internal void Cancelled() { Status = UiText.Get("ImageDownloadCancelled"); NotifyState(); }
    internal void Failed(Exception error) { Status = UiText.Format("ImageDownloadFailed", error.Message); NotifyState(); }
    internal void End() { IsBusy = false; NotifyState(); }
    private void NotifyState()
    {
        foreach (string property in new[] { nameof(IsBusy), nameof(CanDownload), nameof(CanUse), nameof(ImagePath), nameof(Status),
            nameof(Percent), nameof(IsIndeterminate), nameof(ProgressVisibility), nameof(UseVisibility), nameof(StatusVisibility) })
            Notify(property);
    }
}
