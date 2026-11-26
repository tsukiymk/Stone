package com.trebledot.stone.probe;

/** Windows notification history shared by independent foreground-service tests. */
final class NotificationHistoryProbe {
    static native String[] history();
}
