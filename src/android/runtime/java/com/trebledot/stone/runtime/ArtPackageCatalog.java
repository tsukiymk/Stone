package com.trebledot.stone.runtime;

/** Coalesced, asynchronous read model for the native settings application. */
final class ArtPackageCatalog {
    private static final java.util.concurrent.ThreadPoolExecutor worker = new java.util.concurrent.ThreadPoolExecutor(
        1, 1, 0, java.util.concurrent.TimeUnit.MILLISECONDS, new java.util.concurrent.ArrayBlockingQueue<Runnable>(1),
        action -> { Thread thread = new Thread(action, "Stone settings catalog"); thread.setDaemon(true); return thread; },
        new java.util.concurrent.ThreadPoolExecutor.DiscardOldestPolicy());
    static void request(ArtPackageStore store) {
        worker.execute(() -> {
            try { store.exportCatalog(); }
            catch (Exception error) { android.util.Log.w("StonePackages", "Could not update the settings application catalog", error); }
        });
    }
    private ArtPackageCatalog() { }
}
