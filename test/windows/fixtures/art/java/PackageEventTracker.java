package com.trebledot.stone.probe;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import java.util.ArrayList;
import java.util.List;

/** Tracks only the test package, including a quiet interval to detect duplicate sends. */
final class PackageEventTracker implements AutoCloseable {
    final List<Intent> events = new ArrayList<>();
    private final Context context;
    private final BroadcastReceiver receiver;
    PackageEventTracker(Context context, String name) {
        this.context = context;
        receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context owner, Intent intent) {
                if (intent.getData() != null && name.equals(intent.getData().getSchemeSpecificPart())) events.add(new Intent(intent));
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_PACKAGE_ADDED); filter.addDataScheme("package");
        filter.addAction(Intent.ACTION_PACKAGE_REMOVED); filter.addAction(Intent.ACTION_PACKAGE_REPLACED); filter.addAction(Intent.ACTION_PACKAGE_FULLY_REMOVED);
        context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
    }
    void expectCount(int expected) throws Exception {
        CommitProbe.await(() -> events.size() >= expected);
        long until = android.os.SystemClock.uptimeMillis() + 600;
        while (android.os.SystemClock.uptimeMillis() < until) { BroadcastProbe.drain(); Thread.sleep(5); }
        if (events.size() != expected) throw new AssertionError("Package event count: " + events.size() + ", expected " + expected);
    }
    @Override public void close() { context.unregisterReceiver(receiver); }
}
