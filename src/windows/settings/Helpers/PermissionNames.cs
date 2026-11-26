namespace StoneSettings.Helpers;

internal static class PermissionNames
{
    internal static string[]? Validate(string[]? permissions)
    {
        if (permissions is null)
            return null;
        if (permissions.Length > 1024 || permissions.Any(value => string.IsNullOrWhiteSpace(value) || value.Length > 512))
            throw new InvalidDataException("Invalid requested permission list.");
        return permissions.Distinct(StringComparer.Ordinal).ToArray();
    }

    internal static string Label(string permission) => permission switch
    {
        "android.permission.INTERNET" => UiText.Get("PermissionInternet"),
        "android.permission.ACCESS_NETWORK_STATE" => UiText.Get("PermissionNetworkState"),
        "android.permission.ACCESS_WIFI_STATE" => UiText.Get("PermissionWifiState"),
        "android.permission.CHANGE_WIFI_STATE" => UiText.Get("PermissionChangeWifi"),
        "android.permission.CAMERA" => UiText.Get("PermissionCamera"),
        "android.permission.RECORD_AUDIO" => UiText.Get("PermissionMicrophone"),
        "android.permission.ACCESS_COARSE_LOCATION" => UiText.Get("PermissionApproximateLocation"),
        "android.permission.ACCESS_FINE_LOCATION" => UiText.Get("PermissionPreciseLocation"),
        "android.permission.ACCESS_BACKGROUND_LOCATION" => UiText.Get("PermissionBackgroundLocation"),
        "android.permission.POST_NOTIFICATIONS" => UiText.Get("PermissionNotifications"),
        "android.permission.READ_CONTACTS" => UiText.Get("PermissionReadContacts"),
        "android.permission.WRITE_CONTACTS" => UiText.Get("PermissionWriteContacts"),
        "android.permission.READ_CALENDAR" => UiText.Get("PermissionReadCalendar"),
        "android.permission.WRITE_CALENDAR" => UiText.Get("PermissionWriteCalendar"),
        "android.permission.READ_EXTERNAL_STORAGE" => UiText.Get("PermissionReadStorage"),
        "android.permission.WRITE_EXTERNAL_STORAGE" => UiText.Get("PermissionWriteStorage"),
        "android.permission.MANAGE_EXTERNAL_STORAGE" => UiText.Get("PermissionAllFiles"),
        "android.permission.READ_MEDIA_IMAGES" => UiText.Get("PermissionImages"),
        "android.permission.READ_MEDIA_VIDEO" => UiText.Get("PermissionVideo"),
        "android.permission.READ_MEDIA_AUDIO" => UiText.Get("PermissionAudio"),
        "android.permission.BLUETOOTH" => UiText.Get("PermissionBluetoothLegacy"),
        "android.permission.BLUETOOTH_CONNECT" => UiText.Get("PermissionBluetooth"),
        "android.permission.BLUETOOTH_SCAN" => UiText.Get("PermissionBluetoothScan"),
        "android.permission.BLUETOOTH_ADMIN" => UiText.Get("PermissionBluetoothSettings"),
        "android.permission.READ_PHONE_STATE" => UiText.Get("PermissionPhoneState"),
        "android.permission.CALL_PHONE" => UiText.Get("PermissionCallPhone"),
        "android.permission.READ_SMS" => UiText.Get("PermissionReadSms"),
        "android.permission.SEND_SMS" => UiText.Get("PermissionSendSms"),
        "android.permission.REQUEST_INSTALL_PACKAGES" => UiText.Get("PermissionInstallPackages"),
        "android.permission.REQUEST_DELETE_PACKAGES" => UiText.Get("PermissionDeletePackages"),
        "android.permission.WAKE_LOCK" => UiText.Get("PermissionWakeLock"),
        "android.permission.RECEIVE_BOOT_COMPLETED" => UiText.Get("PermissionBoot"),
        "android.permission.FOREGROUND_SERVICE" => UiText.Get("PermissionForegroundService"),
        "android.permission.VIBRATE" => UiText.Get("PermissionVibrate"),
        _ => permission
    };
}
