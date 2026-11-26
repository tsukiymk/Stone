using StoneSettings.Helpers;
using StoneSettings.Models;

namespace StoneSettings.ViewModels;

[WinRT.GeneratedWinRTExposedType]
public sealed partial class PackageConfirmationPageViewModel
{
    public bool IsNotifications
    {
        get;
    }
    public bool IsRemoving
    {
        get;
    }
    public bool IsPreapproval
    {
        get;
    }
    public bool IsUpdating
    {
        get;
    }
    public bool CanApprove
    {
        get;
    }
    public bool IsUnverified
    {
        get;
    }
    public string Heading
    {
        get;
    }
    public string? Package
    {
        get;
    }
    public string? Version
    {
        get;
    }
    public string? Installer
    {
        get;
    }
    public string Initial
    {
        get;
    }
    public string Message
    {
        get;
    }
    public string PrimaryText
    {
        get;
    }
    public string CancelText
    {
        get;
    }
    public string ApplicationName
    {
        get;
    }
    public bool IsFailure { get; }
    public string FailureTitle { get; } = "";
    public string FailureReason { get; } = "";
    public string FailureSuggestion { get; } = "";
    public string FailureDetails { get; } = "";

    internal PackageConfirmationPageViewModel(PackageConfirmationData request, string? preparationMessage,
        PackageOperationResult? failure = null)
    {
        if (request.Operation is not ("install" or "uninstall" or "preapprove" or "notifications"))
            throw new ArgumentException("Unsupported package window.");
        IsRemoving = request.Operation == "uninstall";
        IsPreapproval = request.Operation == "preapprove";
        IsNotifications = request.Operation == "notifications";
        IsFailure = failure is not null;
        IsUpdating = !IsRemoving && !IsPreapproval && !string.IsNullOrEmpty(request.PreviousVersion);
        Heading = IsFailure ? UiText.Get("CannotInstallHeading") : UiText.Format(IsNotifications ? "NotificationHeading" : preparationMessage is not null ? "PreparingInstallHeading"
            : IsPreapproval ? "PreapproveHeading" : IsRemoving ? "UninstallHeading" : IsUpdating ? "UpdateHeading" : "InstallHeading", request.Name);
        Package = string.IsNullOrEmpty(request.PackageName) ? IsFailure ? request.Name : null : UiText.Format("Package", request.PackageName);
        Version = string.IsNullOrEmpty(request.Version) || IsPreapproval ? null
            : UiText.Format("Version", (IsUpdating ? request.PreviousVersion + " → " : "") + request.Version);
        Installer = string.IsNullOrEmpty(request.Installer) ? null : UiText.Format("RequestedBy", request.Installer);
        Initial = System.Globalization.StringInfo.GetNextTextElement(request.Name);
        Message = preparationMessage ?? UiText.Get(IsNotifications ? "NotificationConfirmation" : IsRemoving ? "UninstallConfirmation"
            : IsPreapproval ? "PreapproveConfirmation" : !request.Verified ? "UnverifiedApp" : IsUpdating ? "UpdateConfirmation" : "InstallConfirmation");
        PrimaryText = UiText.Get(IsNotifications || IsPreapproval ? "Allow" : IsRemoving ? "Uninstall" : IsUpdating ? "Update" : "Install");
        CancelText = UiText.Get(IsFailure ? "Close" : IsNotifications ? "Deny" : "Cancel");
        CanApprove = !IsFailure && preparationMessage is null && (IsNotifications || IsRemoving || IsPreapproval || request.Verified);
        IsUnverified = !IsFailure && preparationMessage is null && !IsNotifications && !IsRemoving && !IsPreapproval && !request.Verified;
        ApplicationName = UiText.Get(IsNotifications ? "NotificationTitle" : "InstallerTitle");
        if (failure is not null)
        {
            FailureDetails = failure.Message;
            PackageCompatibilityFailure? compatibility = failure.Compatibility;
            string? kind = compatibility?.IsValid == true ? compatibility.Reason switch
            {
                "native-abi" when compatibility.Required.Length > 0 && compatibility.Available.Length > 0 => "NativeAbi",
                "android-api" when int.TryParse(compatibility.Required, out int required) && required > 0
                    && int.TryParse(compatibility.Available, out int available) && available > 0 => "AndroidApi",
                "image-abi" => "ImageAbi",
                "shared-uid" => "SharedUid",
                "split-apk" => "SplitApk",
                _ => null
            } : null;
            string prefix = "InstallFailure" + kind;
            FailureTitle = UiText.Get(kind is null ? "InstallFailureTitle" : "InstallFailure" + kind + "Title");
            FailureReason = kind is null ? string.IsNullOrWhiteSpace(failure.Message) ? UiText.Get("InstallFailureReason") : failure.Message
                : UiText.Format(prefix + "Reason", compatibility!.Required, compatibility.Available);
            FailureSuggestion = kind is null ? UiText.Get("InstallFailureSuggestion")
                : UiText.Format(prefix + "Suggestion", compatibility!.Required, compatibility.Available);
        }
    }
}
