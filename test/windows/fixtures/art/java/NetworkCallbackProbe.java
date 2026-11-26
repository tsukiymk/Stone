package com.trebledot.stone.probe;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.HandlerThread;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Real image requests must match the host network, including implicit constraints. */
final class NetworkCallbackProbe {
    private static final class Snapshot {
        final int state;
        final Network active;
        final NetworkCapabilities capabilities;
        Snapshot(int state,Network active,NetworkCapabilities capabilities) {
            this.state = state; this.active = active; this.capabilities = capabilities;
        }
    }
    private static Snapshot awaitSnapshot(ConnectivityManager manager,java.lang.reflect.Method host,
            NetworkRequest basic,NetworkRequest internet) throws Exception {
        long start = android.os.SystemClock.uptimeMillis(), deadline = start + 5000;
        Snapshot observed = null;
        do {
            int state = (Integer) host.invoke(null);
            Network active = manager.getActiveNetwork();
            NetworkCapabilities capabilities = manager.getNetworkCapabilities(active);
            observed = new Snapshot(state,active,capabilities);
            // The native snapshot and Android's periodic monitor update independently.
            // Require the real request constraints and a stable native observation.
            if (state == (Integer) host.invoke(null)
                    && basic.canBeSatisfiedBy(capabilities) == ((state & 1) != 0)
                    && internet.canBeSatisfiedBy(capabilities) == ((state & 3) == 3)) {
                System.out.println("INFO image network snapshot matched after " + (android.os.SystemClock.uptimeMillis()-start) + " ms.");
                return observed;
            }
            Thread.sleep(20);
        } while (android.os.SystemClock.uptimeMillis() < deadline);
        throw new AssertionError("Image network request mismatches real host state after snapshot deadline: "
            + observed.state+"; "+basic+"; "+internet+"; "+observed.capabilities);
    }
    static void verify(Context context) throws Exception {
        ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
        java.lang.reflect.Method host = Class.forName("com.trebledot.stone.runtime.ArtConnectivity")
            .getDeclaredMethod("queryHostNetworkState");
        host.setAccessible(true);
        NetworkRequest basic = new NetworkRequest.Builder().build();
        NetworkRequest internet = new NetworkRequest.Builder().addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED).build();
        Snapshot snapshot = awaitSnapshot(manager,host,basic,internet);
        int state = snapshot.state;
        Network active = snapshot.active;
        NetworkCapabilities capabilities = snapshot.capabilities;
        if ((state & 1) == 0) {
            if (active != null || capabilities != null) throw new AssertionError("Disconnected host exposed an active network");
            System.out.println("PASS image network requests reject a disconnected host; connected callbacks require a connected host.");
            return;
        }
        if (manager.getNetworkCapabilities(active) == capabilities)
            throw new AssertionError("Network capability snapshots share the same mutable object");
        NetworkRequest wifiOnly = new NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build();
        if (wifiOnly.canBeSatisfiedBy(capabilities)) throw new AssertionError("Host-owned wired network invented a Wi-Fi transport");
        HandlerThread thread = new HandlerThread("Stone network callback probe"); thread.start();
        AtomicReference<Throwable> error = new AtomicReference<>();
        class Callback extends ConnectivityManager.NetworkCallback {
            final CountDownLatch received = new CountDownLatch(1);
            private Network available;
            @Override public void onAvailable(Network network) { available = network; }
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities current) {
                if (!network.equals(active) || !network.equals(available) || !basic.canBeSatisfiedBy(current))
                    error.compareAndSet(null,new AssertionError("Network callback arrived out of order or failed the real request: "+current));
                received.countDown();
            }
        }
        Callback defaults = new Callback(), filtered = new Callback();
        boolean registeredDefault = false, registeredFilter = false;
        try {
            Handler handler = new Handler(thread.getLooper());
            manager.registerDefaultNetworkCallback(defaults,handler); registeredDefault = true;
            manager.registerNetworkCallback(basic,filtered,handler); registeredFilter = true;
            if (!defaults.received.await(3,TimeUnit.SECONDS) || !filtered.received.await(3,TimeUnit.SECONDS) || error.get() != null)
                throw new AssertionError("Real default and matching network callbacks did not complete",error.get());
        } finally {
            try { if (registeredFilter) manager.unregisterNetworkCallback(filtered); }
            finally {
                try { if (registeredDefault) manager.unregisterNetworkCallback(defaults); }
                finally { thread.quitSafely(); thread.join(3000); }
            }
        }
        if (thread.isAlive()) throw new AssertionError("Network callback thread did not retire");
        System.out.println("PASS image network requests: implicit platform constraints, actual host connection/Internet state, isolated snapshots, wired transport and ordered default/filtered callbacks.");
    }
}
