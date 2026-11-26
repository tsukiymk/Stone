# Windows desktop integration

Stone connects Android requests to Windows Shell, the clipboard, system notifications, and desktop windows. ART owns object and permission semantics. Service owns host state shared across Clients, while Client retains interactions tied to its own HWND.

## Notifications and action recovery

StoneService owns Windows Toast notifications, notification history, and activation queues. Requests bind to the same user, Windows session, installation executable, Client process, package name, and data directory. Cancelling or replacing a notification invalidates its previous route.

Supported notification text, progress, buttons, and PendingIntent snapshots can be persisted and recovered within the current login session. After the application exits, a click creates Client using trusted launch profiles, and ART dispatches the original Intent. Complete Notification objects and Binder objects do not cross process boundaries.

After Service replacement, supported notifications can be rebuilt silently without repeating sounds. One-shot actions are persistently consumed before dispatch. A crash after claiming an action can lose the callback; exactly-once delivery is not guaranteed. Foreground services and non-persistent actions may require a live Client. Recovery across boot or a new login is not fully implemented.

## Clipboard

ClipboardManager retains Android descriptions and listeners. Service's message window reads and writes Windows Unicode text. Only a single plain-text item is supported: at most 8192 UTF-16 characters in the body and 1024 in the label. Multiple items, rich text, files, URIs, and arbitrary Parcelable values are unsupported.

A foreground Client observes sequence numbers in the background and caches descriptions. Body reads, copying, and clearing are explicit requests. Description queries do not proactively read the body, avoiding external delayed rendering. Android background sessions neither read nor modify the body. The Windows clipboard does not maintain separate contents per APK.

Text uses CF_UNICODETEXT; restricted labels and PersistableBundle descriptions use a custom Stone format. Body and description publication is not atomic, so partial failure may alter previous contents. Submitted writes or clears with uncertain results are not replayed automatically.

## Share panel

For a single text/plain SEND/CHOOSER request, ART checks the body and title, and Client opens the Windows share panel using its own HWND. If the entire body is an ordinary HTTP/HTTPS URL, a WebLink can also be supplied. A literal file: value remains plain text.

A single APK content URI can be prepared as read-only Windows StorageItems through background reads and a bounded copy cache. Grants, provider identity, and actual files are checked. Android or host private paths are not passed directly to recipients.

Multiple items, other file types, HTML, mail metadata, and complex chooser flows are unsupported. Stone neither selects recipients nor sends automatically. Panel creation, request preparation, user cancellation, and actual receipt are distinct states; complete delivery remains limited.

## Web and Shell entry points

An ordinary HTTP/HTTPS VIEW from a foreground application can asynchronously open the Windows default browser through Service. Java and Service both validate the URI, rejecting file paths, other protocols, invalid addresses, and unsupported payloads. URLs are not interpreted through Shell commands.

Success means only that Windows activation was queued, not that the page loaded. Additional payloads, mail, custom protocols, and returned results are unsupported. Service manages installed-package Start menu entries, stable taskbar identity, and stone:// activation. See [package management](../android/packages.md) for details.
