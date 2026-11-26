# Application lifecycle and Activity routing

Application, the main Looper, Activity, Service, and Binder objects in Client are managed by image ART. The window thread coordinates Win32 messages, input, and software drawing; Android components are not executed by StoneService.

## Foreground and background

Opening an APK creates its Application and launcher Activity. `--background` creates only the Application and required background components, without automatically opening the launcher page. Closing the window destroys Activities and releases drawing resources. The process remains alive while a Service, JobService, or supported wake request is active, then obtains Service confirmation before idle exit when no work remains.

Application execution locks constrain duplicate launches by data root and package identity. Persistent launch profiles bind code, data, image, and revocable identifiers. Service rechecks identity before restoring a background Client; old notifications, uninstalled packages, and revoked profiles cannot be used to launch another application.

## Pages, recreation, and input

A Client retains an Activity back stack. Background pages and their popups are hidden and stop receiving input. Resources are released when a page is destroyed or the whole window closes.

Application-requested recreation and host configuration changes use the image ActivityThread recreation path: save instance state and the original Intent, retain non-configuration instances and ViewModels, destroy the old page, and restore the new one. State is read with the APK ClassLoader. Failure cleanup does not destroy the same instance twice.

Back handling uses the image Activity's finish callback and preserves any result code and data. Ordinary launcher root pages follow Stone's minimize path; root pages invoked across Clients are not mistaken for launcher roots.

## Queries and cross-application launching

The image IntentFilter matches Activity/alias action, category, MIME, scheme, authority, path, and scheme-specific-part. Component enabled state, export status, permissions, and the default category are checked separately. Callers cannot alter cached manifest data by modifying returned objects.

Supported cross-Client Activity requests are authenticated by Service. Source and target use the same data root and image identity. Caller identity comes from trusted component and result relationships, not Intent extras. Routing uses process generations to prevent PID reuse, and Service replacement revokes temporary result relationships.

Cross-package payloads use a bounded value-only format supporting common strings, scalars, and byte[]. Arbitrary Parcelable/Serializable values from external APKs are not deserialized. Task flags, URI grants, ClipData, custom permissions, and nonstandard launch modes are limited by the supported routing scope.

This is not a complete Android task manager. Full cross-process task stacks, arbitrary process models, the system Resolver, App Links domain verification, and package visibility policies are not fully implemented.

## Local Services

Image ActivityThread handles supported local Service startup, binding, rebinding, stopping, and destruction. Foreground services associate notifications with ID ownership; Windows notification presentation is handled by [host integration](../windows/integration.md). Basic lifecycle support does not establish support for system-type permissions, service crash recovery, or long-term background behavior.
