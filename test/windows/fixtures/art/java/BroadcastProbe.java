package com.trebledot.stone.probe;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.List;

final class BroadcastProbe {
    static void drain() throws Exception {
        boolean[] done = {false};
        new Handler(Looper.getMainLooper()).post(() -> done[0] = true);
        java.lang.reflect.Method once = Looper.class.getDeclaredMethod("loopOnce", Looper.class, long.class, int.class);
        once.setAccessible(true);
        long previous = android.os.Binder.clearCallingIdentity();
        long identity = android.os.Binder.clearCallingIdentity();
        try {
            // Zero forces a slow-message log for every dispatch. Match the
            // normal Looper default so diagnostics do not dominate UI timings.
            while (!done[0]) if (!(Boolean) once.invoke(null, Looper.getMainLooper(), identity, -1))
                throw new AssertionError("ART main queue stopped");
        } finally { android.os.Binder.restoreCallingIdentity(previous); }
    }

    static void run(Context context) throws Exception {
        String action = context.getPackageName() + ".STONE_PENDING_PROBE";
        List<Intent> received = new ArrayList<>();
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                if (Looper.myLooper() != Looper.getMainLooper()) throw new AssertionError("registered broadcast thread");
                PendingResult result = goAsync();
                if (result == null) throw new AssertionError("registered broadcast pending result");
                received.add(new Intent(intent));
                result.finish();
            }
        };
        context.registerReceiver(receiver, new IntentFilter(action), Context.RECEIVER_NOT_EXPORTED);
        try {
            Intent base = new Intent(action).setPackage(context.getPackageName()).putExtra("base", "original");
            PendingIntent mutable = PendingIntent.getBroadcast(context, 41, base, PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            PendingIntent immutable = PendingIntent.getBroadcast(context, 42, base, PendingIntent.FLAG_IMMUTABLE);
            if (!context.getPackageName().equals(mutable.getCreatorPackage()) || mutable.isImmutable() || !immutable.isImmutable())
                throw new AssertionError("PendingIntent identity and mutability");
            context.sendBroadcast(base);
            mutable.send(context, 0, new Intent().putExtra("status", 42));
            immutable.send(context, 0, new Intent().putExtra("status", 43));
            drain();
            if (received.size() != 3 || received.get(0).hasExtra("status") || received.get(1).getIntExtra("status", 0) != 42
                    || received.get(2).hasExtra("status")) throw new AssertionError("broadcast delivery and immutable fill-in");
            PendingIntent updated = PendingIntent.getBroadcast(context, 41, new Intent(base).putExtra("base", "updated"),
                PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            if (!mutable.equals(updated) || !mutable.equals(PendingIntent.getBroadcast(context, 41, base, PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_NO_CREATE)))
                throw new AssertionError("PendingIntent update and lookup identity");
            mutable.send(); drain();
            if (!"updated".equals(received.get(3).getStringExtra("base"))) throw new AssertionError("PendingIntent extras update");
            PendingIntent replaced = PendingIntent.getBroadcast(context, 41, base, PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_CANCEL_CURRENT);
            try { mutable.send(); throw new AssertionError("cancelled PendingIntent remained usable"); }
            catch (PendingIntent.CanceledException expected) { }
            replaced.cancel(); immutable.cancel();
            PendingIntent once = PendingIntent.getBroadcast(context, 43, base, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_ONE_SHOT);
            once.send(); drain();
            try { once.send(); throw new AssertionError("one-shot PendingIntent sent twice"); }
            catch (PendingIntent.CanceledException expected) { }
            Class<?> manifest = Class.forName("com.trebledot.stone.cit.components.InstallResultReceiver");
            List<?> statuses = (List<?>) manifest.getField("received").get(null);
            int before = statuses.size();
            PendingIntent explicit = PendingIntent.getBroadcast(context, 44, new Intent(action).setComponent(new ComponentName(context.getPackageName(), manifest.getName())),
                PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_CANCEL_CURRENT);
            explicit.getIntentSender().sendIntent(context, 0, new Intent().putExtra("status", 99), null, null);
            drain(); drain();
            if (statuses.size() != before + 1 || ((Intent) statuses.get(before)).getIntExtra("status", 0) != 99 || !manifest.getField("valid").getBoolean(null))
                throw new AssertionError("manifest receiver and goAsync delivery");
            explicit.cancel();
            int registered = received.size();
            context.registerReceiver(receiver, new IntentFilter(action + ".SECOND"), Context.RECEIVER_NOT_EXPORTED);
            context.registerReceiver(receiver, new IntentFilter(action), Context.RECEIVER_NOT_EXPORTED);
            context.sendBroadcast(base);
            context.sendBroadcast(new Intent(action + ".SECOND").setPackage(context.getPackageName()));
            drain();
            if (received.size() != registered + 2) throw new AssertionError("multiple receiver filters or duplicate delivery");
        } finally { context.unregisterReceiver(receiver); }
        int size = received.size();
        context.sendBroadcast(new Intent(action).setPackage(context.getPackageName())); drain();
        if (received.size() != size) throw new AssertionError("unregistered broadcast receiver received an intent");
        System.out.println("ART delivered registered and manifest broadcasts, goAsync, mutable/immutable PendingIntents, updates, cancellation and one-shot sends.");
    }
}
