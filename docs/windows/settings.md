# Stone settings

StoneSettings is a self-contained WinUI desktop application with a welcome page, system image selection, application management, permissions, and confirmation interfaces. See [building from source](../development/build.md) for build and publication dependencies and [Windows packages](packaging.md) for MSI/MSIX distribution.

## Scope

The welcome and system management pages download, select, and validate complete Android system images. Application, notification, and permission pages display background query results and submit management requests. The settings application does not load ART, execute Android components, or perform package installation transactions.

APK launching and ART AOT/JIT use the [command line and data-root configuration](../usage.md#runtime-configuration-by-data-root). Settings pages do not expose these interfaces or rewrite `runtime-features.properties`.

## Downloading an Android image

The welcome page presents two setup choices: a recommended download and importing an existing `system.img`.
Preparation progress and recoverable errors appear inline.

The welcome page and System page provide **Download image**, defaulting to the stable AOSP Android API 37.0 x86_64 image. StoneSettings reads Google's [official image catalog](https://dl.google.com/android/repository/sys-img/android/sys-img2-3.xml) for the latest package revision, archive size, checksum, and SDK license. The download starts after accepting the displayed license.

Images are saved under `%LOCALAPPDATA%\Trebledot Productions\Stone\System Images\android-37.0-x86_64-r<revision>\system.img`. Progress covers download, integrity verification, and extraction; **Cancel download** cleans up the current temporary files. Only `system.img` is extracted to a fixed path. The downloaded image's SDK license and source/checksum receipt are retained alongside it. Existing downloads are checked against their receipt and image SHA-256 before reuse; modified images fail explicitly rather than being overwritten.

Choose **Use this image** after download to run the existing full image validation/preparation and save the selection for the current data root. Preparation uses the bundled native `StoneImageTool.exe` without a Windows JDK or JVM. A failed download or validation does not change the selected image, and downloading does not establish APK compatibility. Windows ARM64 APK execution remains unsupported.

Android's default data root is `%LOCALAPPDATA%\Trebledot Productions\Stone\User Data`. Image downloads use the sibling `System Images` directory, while process and Android text logs use `Logs`. An explicit `--data-dir` changes Android data and caches while keeping the default image download directory unchanged.

## Interface languages

The welcome page, settings pages, installation/removal confirmation, and notification permission confirmation support Simplified Chinese (`zh-CN`), English (`en-US`), and Japanese (`ja-JP`). They follow the Windows display language by default, with missing language resources falling back to English. Language resources are built and published with `StoneSettings.pri`.

Use `settings/StoneSettings.exe --language zh-CN`, `--language en-US`, or `--language ja-JP` to select the current process's interface language. `--language system` follows Windows again. The argument is not saved and does not change Windows or Android application languages. Confirmation windows launched separately by Client still follow Windows by default. Overrides are read only at process startup; restart the settings application to change language.

Static XAML text and accessible names use `x:Uid`. Dynamic text reads the same `Resources/<language>/Resources.resw` catalogs through `Helpers/UiText.cs`. Text containing application names, versions, or counts uses complete format strings; numbers follow the user's regional format. When adding a language, preserve all resource keys and format placeholders, and update the supported overrides in `UiText.Configure`.

## Layout and host discovery

The program and dependencies are under `bin/<architecture>/<configuration>/settings/`. `StoneClient.exe` and `StoneService.exe` are located in the parent directory, alongside the ART bootstrap JAR and native image tool and the default `system.img`. Image selection first uses startup arguments, then the path saved in the data root, and finally the default image. Client read-only or transactional endpoints handle application lists and management requests.

Initial window dimensions are defined in DIP, converted for the current DPI, and constrained to the work area. WinUI and PerMonitorV2 handle later DPI changes without continually overriding user resizing.

## Application architecture

The application separates window hosts, Shell navigation, and page ViewModels. The main window hosts `Views/ShellPage`, using a Frame to switch between system, application, notification, and permission pages. The welcome window hosts a separate welcome page. Installation and native confirmation windows host a shared confirmation page.

| Directory | Responsibilities |
| --- | --- |
| Root `*Window.xaml` files | HWND, title bars, and startup coordination |
| `Views/` | Page XAML, control events, and visual state |
| `ViewModels/` | Only the corresponding `*PageViewModel.cs` classes; lists, filtering, busy state, and page actions |
| `Models/` | Application catalogs, confirmation requests, transaction results, and JSON serialization types |
| `Services/` | Application catalog reads, Client requests, image validation, and permission persistence; preserve identity rechecks, cross-process locks, and atomic replacement |
| `Helpers/`, `Utils/` | Localization, property notifications, window sizing, and host layout helpers |
| `Styles/`, `Resources/` | Shared styles and language resources |

## Application and permission management

Installed application summaries, current running state, and host policies come from background queries. The interface does not read ART objects directly. Supported notification and all-files access switches are saved per application; undeclared or unconnected capabilities are not shown as grantable permissions.

Application details include an **Open app** button beside the application icon. It starts StoneClient with the registered package name, current data root, and selected system image; Client resolves and validates the current installed version. The button is disabled while the backend is unavailable. A queued launch does not establish that application startup succeeded; Client presents subsequent startup errors.

Application lists, details, notification entries, permission selectors, and installation/removal confirmations display the APK's application icon. The image's resource loader rasterizes bitmap, vector, and adaptive drawables without loading APK application classes. Catalog refresh writes an atomic PNG cache under `data/system/stone-packages/icons/<package>.png` in the Android data directory; Settings derives this path from a validated package name. Confirmation carries a bounded base64 PNG from the verified candidate, retaining the existing 64 KB transport limit. PNGs are limited to 32 KB and 256 pixels per dimension. Missing or unreadable icons use a neutral application glyph, and a recycled list item cannot retain another application's image. Icons are presentation data and never establish package identity or authorization.

Client initializes the image's software graphics natives for every package-management operation, including listing and removal, before reading application icons. These background operations need no Android application window or Windows JVM.

File grants bind to UID and private-directory identity, and running Clients refresh access policy. Uninstallation, directory identity changes, and reinstallation follow their respective grant rules. Display names alone cannot restore permissions.

When an application requests notification permission, a centered rounded dialog follows the light/dark theme without a desktop overlay. Its initial size is 548×280 DIP, constrained to the display work area; long content can scroll. The equal-width bottom buttons are "Allow" and "Don't allow", with "Don't allow" focused by default. Escape, closing, cancellation, or caller disconnection deny permission and close the dialog. The dialog participates in normal Windows application switching.

## Installation, update, and removal confirmation

Client performs APK preparation, image/package metadata reads, and final transactions in the background. The settings window displays preparation state, candidate information, and actual confirmation results. Existing packages distinguish installation from update; previous-version information comes from a background package-store snapshot.

Preparation or transaction failures show a terminal **Cannot install this app** page with a prominent reason, a suggested next step, expandable original error details, and a single **Close** action. This state hides preparation progress, permission headings, and installation approval. Known API/ABI, unsupported image architecture, shared-UID, and split-APK failures are localized in all three interface languages. API/ABI explanations include both the APK requirements and selected-image capabilities. Unknown failures retain their original diagnostic instead of being labeled as a known compatibility problem.

The bounded version-1 package result may carry an optional `compatibility` object with `reason`, `required`, and `available` fields. Settings uses these fields for presentation only; they never authorize installation. Older results without the object use the generic failure page. Reasons are limited to 64 characters and each value to 4096 characters within the existing 64 KB result-line limit; malformed fields are rejected.

The installation/update information section lists every requested permission from the verified candidate's `PackageInfo.requestedPermissions`. Common Android permissions use localized names; unknown/custom permissions retain their complete identifiers, and tooltips expose known permissions' identifiers. A verified empty list displays "No permissions"; unavailable metadata is reported separately. The list is informational and does not grant permissions. The restricted confirmation frame remains limited to 64 KB; oversized requests fail instead of silently omitting permissions.

Confirmation returns to the requester through an asynchronous restricted protocol. ART/package storage rechecks the nonce, caller, signatures, and current package state. Cancellation, closing, and preparation failure do not approve requests. Preapproval does not label an APK that has not been downloaded as signature-verified.

Settings presentation, native confirmation, and package transactions are separate layers. See [package management](../android/packages.md) and [storage](../android/storage.md) for permission, application data, and installation identity boundaries.

## Validation

Static validation scripts are under `test/windows/integration/settings/`. Invoke them from the repository root with `pwsh -File <script-path>`.

| Script | Validation scope |
| --- | --- |
| [CheckLocalization.ps1](../../test/windows/integration/settings/CheckLocalization.ps1) | Language resource keys, placeholders, XAML identifiers, and code references |
| [CheckArchitecture.ps1](../../test/windows/integration/settings/CheckArchitecture.ps1) | Page and ViewModel structure and settings page scope |

These checks require no system image and perform neither APK installation nor real application authorization.

### Windows UI Automation

The [UI Automation tests](../../test/windows/settings/ui-automation-tests.cs) run in a separate .NET process and drive the normally launched application through Windows UI Automation. The [driver](../../test/windows/settings/ui-automation-driver.cs) locates controls within the owned process's window by stable `AutomationProperties.AutomationId` values. It does not reference the application's assemblies or call its controls directly.

Build and run from the repository root in an unlocked, interactive Windows x64 desktop session. The driver fails before starting application windows if it detects a locked session or cannot access the interactive desktop:

```powershell
cmake --preset x64 -DSTONE_TESTS=ON
cmake --build build/x64 --config Debug --target stone-settings-ui-tests -- /restore
```

After both commands succeed:

```powershell
Push-Location bin/x64/Debug
try {
    ctest --test-dir test/ctest/Debug -C Debug -R '^stone-settings-ui-automation$' --no-tests=error --output-on-failure
} finally {
    Pop-Location
}
```

`stone-windows-tests` also builds this driver and its StoneSettings/StoneClient dependencies. The CTest case is Debug-only, serial, and labeled `settings;ui;integration`; `-L unit` does not execute it. No Appium, WinAppDriver service, system image, or APK download is required.

The suite covers localization, navigation, backend failure presentation, permission persistence and isolation, and confirmation decisions. A synthetic catalog, an invalid image, and protocol-only peers exercise UI and request handling without executing or installing an APK. These inputs do not validate successful image preparation or installation.

Related component tests cover separate contracts: `stone-settings-package-ui` checks result decoding, size limits, localized failure presentation, and launch arguments without XAML; `stone-settings-confirmation` checks protocol identity, cancellation, disconnect, timeout, and replay handling. Refer to the test sources for individual scenarios.

Each run creates a fresh directory under `bin/x64/Debug/test/settings/ui-automation/run/` containing isolated profiles, TEMP/TMP, LOCALAPPDATA, logs, PNG screenshots, UIA tree dumps, and a pass/failure record. Only processes started by the driver are closed or terminated. Screenshots are evidence for review, not automated visual comparisons. Successful APK installation, valid image selection, rendering across themes/DPI, Android callbacks, and Windows notification delivery require separate acceptance tests.
