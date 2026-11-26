using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.ViewModels;
using Windows.System;

namespace StoneSettings.Views;

// Shared content for installation and native package confirmation windows.
public sealed partial class PackageConfirmationPage : UserControl
{
    private readonly TaskCompletionSource<ContentDialogResult> decision = new();
    private readonly bool notifications;
    public PackageConfirmationPageViewModel ViewModel
    {
        get;
    }
    internal Task<ContentDialogResult> Decision => decision.Task;
    internal Button CancelButton => CloseButton;
    internal ProgressRing? PreparationRing
    {
        get;
    }

    internal PackageConfirmationPage(PackageConfirmationData request, string? preparationMessage = null,
        PackageOperationResult? failure = null)
    {
        ViewModel = new(request, preparationMessage, failure);
        InitializeComponent();
        notifications = ViewModel.IsNotifications;
        HeadingText.Text = ViewModel.Heading;
        PackageTypeText.Text = UiText.Get("AndroidApp");
        SetDetail(PackageNameText, ViewModel.Package);
        SetDetail(VersionText, ViewModel.Version);
        SetDetail(InstallerText, ViewModel.Installer);
        AppIcon.IconPng = request.IconPng;
        AutomationProperties.SetName(AppIcon, UiText.Format("AppIcon", request.Name));
        InformationHeading.Text = UiText.Get(ViewModel.IsRemoving ? "UninstallInformation" : "InstallInformation");
        MessageText.Text = ViewModel.Message;
        if (!ViewModel.IsFailure && request.Operation == "install" && request.Verified && preparationMessage is null)
        {
            string[]? permissions = PermissionNames.Validate(request.RequestedPermissions);
            MessageText.Visibility = Visibility.Collapsed;
            PermissionList.Visibility = Visibility.Visible;
            if (permissions is null || permissions.Length == 0)
                PermissionList.Children.Add(new TextBlock { Text = UiText.Get(permissions is null ? "PermissionsUnavailable" : "NoPermissions"),
                    TextWrapping = TextWrapping.Wrap, Style = ThemeStyle("PackageSecondaryText") });
            else
                foreach (string permission in permissions)
                {
                    TextBlock item = new() { Text = PermissionNames.Label(permission), TextWrapping = TextWrapping.Wrap };
                    ToolTipService.SetToolTip(item, permission);
                    PermissionList.Children.Add(item);
                }
        }
        if (!ViewModel.IsFailure && preparationMessage is not null)
        {
            PreparationRing = PreparationProgress;
            PreparationProgress.Visibility = Visibility.Visible;
            PreparationProgress.IsActive = true;
        }
        MessageText.Style = ThemeStyle(ViewModel.IsUnverified ? "PackageErrorText" : "PackageSecondaryText");
        PrimaryButton.Content = ViewModel.PrimaryText;
        PrimaryButton.IsEnabled = ViewModel.CanApprove;
        CancelButton.Content = ViewModel.CancelText;
        if (ViewModel.IsFailure)
        {
            InformationHeading.Visibility = MessageText.Visibility = PrimaryButton.Visibility = Visibility.Collapsed;
            FailurePanel.Visibility = Visibility.Visible;
            FailureInfo.Title = ViewModel.FailureTitle;
            FailureInfo.Message = ViewModel.FailureReason;
            FailureSuggestion.Text = ViewModel.FailureSuggestion;
            FailureDetails.Header = UiText.Get("ErrorDetails");
            FailureDetailsText.Text = ViewModel.FailureDetails;
            FailureDetails.Visibility = string.IsNullOrWhiteSpace(ViewModel.FailureDetails) ? Visibility.Collapsed : Visibility.Visible;
            CancelButton.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        }
        if (notifications)
        {
            LayoutRoot.Padding = new Thickness(0);
            LayoutRoot.ClearValue(Grid.BackgroundProperty);
            LayoutRoot.Style = ThemeStyle("PermissionDialogSurface");
            Body.Margin = new Thickness(24);
            Body.Spacing = 16;
            HeadingText.FontSize = 20;
            PackageTypeText.Visibility = AppIcon.Visibility = InformationHeading.Visibility = AboutButton.Visibility = Visibility.Collapsed;
            Identity.ColumnSpacing = 0;
            IconColumn.Width = new GridLength(0);
            ActionFooter.Style = ThemeStyle("PermissionDialogFooter");
            ActionFooter.Margin = new Thickness(0);
            ActionFooter.Padding = new Thickness(24, 16, 24, 16);
            Commands.HorizontalAlignment = HorizontalAlignment.Stretch;
            foreach (var column in Commands.ColumnDefinitions)
                column.Width = new GridLength(1, GridUnitType.Star);
            // Keep denial as the initial focus and visual default. Both choices
            // have the same width, including at smaller display sizes.
            CancelButton.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
            PrimaryButton.HorizontalAlignment = CancelButton.HorizontalAlignment = HorizontalAlignment.Stretch;
            PrimaryButton.MinWidth = CancelButton.MinWidth = 0;
            PrimaryButton.MinHeight = CancelButton.MinHeight = 32;
        }
        else
        {
            PrimaryButton.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        }

        string applicationName = ViewModel.ApplicationName;
        AboutHeading.Text = UiText.Get("About");
        string applicationVersion = System.Reflection.CustomAttributeExtensions
            .GetCustomAttribute<System.Reflection.AssemblyInformationalVersionAttribute>(typeof(App).Assembly)
            ?.InformationalVersion ?? typeof(App).Assembly.GetName().Version!.ToString();
        AboutVersionText.Text = UiText.Format("AboutVersion", applicationName, applicationVersion);
        ProjectLink.Content = UiText.Get("ProjectHome");
        NoticesLink.Content = UiText.Get("ThirdPartyNotices");
        NoticesLink.IsEnabled = File.Exists(Path.Combine(AppContext.BaseDirectory, "THIRD_PARTY_NOTICES.txt"));
        AutomationProperties.SetName(AboutButton, UiText.Format("AboutApplication", applicationName));
        ToolTipService.SetToolTip(AboutButton, UiText.Format("AboutApplication", applicationName));
    }

    private void SetDetail(TextBlock target, string? text)
    {
        target.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
        if (text is not null)
        {
            target.Text = text;
        }
    }

    private void PrimaryClicked(object sender, RoutedEventArgs args)
    {
        if (ViewModel.CanApprove)
            decision.TrySetResult(ContentDialogResult.Primary);
    }

    private void CancelClicked(object sender, RoutedEventArgs args) => decision.TrySetResult(ContentDialogResult.None);

    private void EscapeInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    {
        decision.TrySetResult(ContentDialogResult.None);
        args.Handled = true;
    }

    private void PageLoaded(object sender, RoutedEventArgs args) =>
        CancelButton.Focus(notifications ? FocusState.Keyboard : FocusState.Programmatic);

    private async void NoticesClicked(object sender, RoutedEventArgs args)
    {
        try
        {
            string noticesPath = Path.Combine(AppContext.BaseDirectory, "THIRD_PARTY_NOTICES.txt");
            await Launcher.LaunchFileAsync(await Windows.Storage.StorageFile.GetFileFromPathAsync(noticesPath));
        }
        catch (Exception error)
        {
            System.Diagnostics.Debug.WriteLine(error);
        }
    }

    private Style ThemeStyle(string resource) => (Style)Resources[resource];
}
