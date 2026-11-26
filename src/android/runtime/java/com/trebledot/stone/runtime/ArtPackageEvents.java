package com.trebledot.stone.runtime;

import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.util.Log;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Properties;
import java.util.Set;
import java.util.TreeSet;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Durable package changes, consumed once per live application process. */
final class ArtPackageEvents implements AutoCloseable {
    static final int RETAINED = 256;
    private final ArtPackageStore store;
    private final ArtBroadcasts broadcasts;
    private final String owner;
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor(action -> {
        Thread thread = new Thread(action, "Stone package changes"); thread.setDaemon(true); return thread;
    });
    private Snapshot previous;
    private boolean warned;

    static final class Snapshot {
        final String identity, epoch;
        final long sequence;
        final Properties state;
        Snapshot(String identity, Properties source) throws IOException {
            this.identity = identity;
            state = (Properties) source.clone();
            epoch = source.getProperty("change.epoch", "");
            sequence = sequence(source);
            if (sequence != 0 && !uuid(epoch)) throw new IOException("Invalid package change epoch");
        }
    }
    private static boolean uuid(String value) { try { return UUID.fromString(value).toString().equals(value); } catch (IllegalArgumentException error) { return false; } }
    private static long sequence(Properties state) throws IOException {
        try {
            long value = Long.parseLong(state.getProperty("change.sequence", "0"));
            if (value < 0) throw new NumberFormatException();
            return value;
        } catch (NumberFormatException error) { throw new IOException("Invalid package change sequence", error); }
    }
    static void append(Properties state, String kind, String name, int uid) throws IOException {
        long sequence;
        try { sequence = Math.addExact(sequence(state), 1); }
        catch (ArithmeticException error) { throw new IOException("Package change sequence exhausted", error); }
        if (!state.containsKey("change.epoch")) state.setProperty("change.epoch", UUID.randomUUID().toString());
        if (!uuid(state.getProperty("change.epoch"))) throw new IOException("Invalid package change epoch");
        state.setProperty("change.sequence", Long.toString(sequence));
        state.setProperty("change.event." + sequence, kind + "\t" + name + "\t" + uid);
        if (sequence > RETAINED) state.remove("change.event." + (sequence - RETAINED));
    }

    private static final class Event {
        final String kind, name;
        final int uid;
        Event(String kind, String name, int uid) throws IOException {
            if (!(kind.equals("add") || kind.equals("replace") || kind.equals("remove"))
                    || !name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+") || uid < 10000 || uid > 19999)
                throw new IOException("Invalid package change event");
            this.kind = kind; this.name = name; this.uid = uid;
        }
        static Event decode(String value) throws IOException {
            if (value == null) throw new IOException("Package change journal has a gap");
            String[] fields = value.split("\t", -1);
            if (fields.length != 3) throw new IOException("Invalid package change event");
            try { return new Event(fields[0], fields[1], Integer.parseInt(fields[2])); }
            catch (NumberFormatException error) { throw new IOException("Invalid package change UID", error); }
        }
    }

    ArtPackageEvents(ArtPackageStore store, ArtBroadcasts broadcasts, String owner) throws IOException {
        this.store = store; this.broadcasts = broadcasts; this.owner = owner;
        previous = store.changes(null); // Baseline before Application.onCreate; no historical replay on startup.
    }
    void start() { worker.scheduleWithFixedDelay(this::poll, 250, 250, TimeUnit.MILLISECONDS); }
    @Override public void close() { worker.shutdownNow(); }

    private synchronized void poll() {
        try {
            Snapshot current = store.changes(previous);
            if (current == previous) return;
            List<Event> events = new ArrayList<>();
            Set<String> covered = new HashSet<>();
            boolean continuous = (current.epoch.equals(previous.epoch) || previous.sequence == 0 && previous.epoch.isEmpty())
                && current.sequence >= previous.sequence && current.sequence - previous.sequence <= RETAINED;
            if (continuous) try {
                for (long sequence = previous.sequence; sequence < current.sequence;) {
                    Event event = Event.decode(current.state.getProperty("change.event." + (++sequence)));
                    events.add(event); covered.add(event.name);
                }
            } catch (IOException gap) { continuous = false; events.clear(); covered.clear(); }
            if (!continuous) Log.w("StonePackages", "Package change journal reset or gap; reconciling current state");
            // Also handles writes by an older Stone version with no event journal.
            reconcile(previous.state, current.state, covered, events);
            for (Event event : events) deliver(event);
            previous = current;
            warned = false;
        } catch (Exception error) {
            if (!warned) Log.w("StonePackages", "Could not observe package changes; will retry", error);
            warned = true;
        }
    }

    private static Set<String> names(Properties state) {
        Set<String> result = new TreeSet<>();
        for (String key : state.stringPropertyNames()) {
            if (key.startsWith("package.")) result.add(key.substring(8));
            if (key.startsWith("removing.")) result.add(key.substring(9));
        }
        return result;
    }
    private static String encoded(Properties state, String name) { return state.getProperty("package." + name, state.getProperty("removing." + name)); }
    private static void reconcile(Properties before, Properties after, Set<String> covered, List<Event> events) throws IOException {
        Set<String> names = names(before); names.addAll(names(after));
        for (String name : names) {
            if (covered.contains(name)) continue;
            if (after.containsKey("removing." + name)) continue; // Cleanup has not committed its removal event yet.
            String old = encoded(before, name), next = encoded(after, name);
            if (java.util.Objects.equals(old, next)) continue;
            PackageInfo previous = old == null ? null : ArtPackageStore.changeInfo(old, name);
            PackageInfo current = next == null ? null : ArtPackageStore.changeInfo(next, name);
            if (previous != null && current != null && previous.applicationInfo.uid == current.applicationInfo.uid)
                events.add(new Event("replace", name, current.applicationInfo.uid));
            else {
                if (previous != null) events.add(new Event("remove", name, previous.applicationInfo.uid));
                if (current != null) events.add(new Event("add", name, current.applicationInfo.uid));
            }
        }
    }

    private void send(Intent intent) {
        try { broadcasts.dispatch(intent, null, 1000, "android"); }
        catch (ReflectiveOperationException | RuntimeException error) { Log.w("StonePackages", "Could not deliver package change", error); }
    }
    private void deliver(Event event) {
        Intent intent = new Intent().setData(Uri.fromParts("package", event.name, null)).putExtra(Intent.EXTRA_UID, event.uid);
        if (event.kind.equals("remove")) {
            intent.putExtra(Intent.EXTRA_DATA_REMOVED, true).putExtra(Intent.EXTRA_REPLACING, false);
            send(new Intent(intent).setAction(Intent.ACTION_PACKAGE_REMOVED));
            send(new Intent(intent).setAction(Intent.ACTION_PACKAGE_FULLY_REMOVED));
            send(new Intent(Intent.ACTION_UID_REMOVED).putExtra(Intent.EXTRA_UID, event.uid));
        } else {
            boolean replacing = event.kind.equals("replace");
            intent.putExtra(Intent.EXTRA_REPLACING, replacing);
            if (replacing) send(new Intent(intent).setAction(Intent.ACTION_PACKAGE_REMOVED).putExtra(Intent.EXTRA_DATA_REMOVED, false));
            send(new Intent(intent).setAction(Intent.ACTION_PACKAGE_ADDED));
            if (replacing) {
                send(new Intent(intent).setAction(Intent.ACTION_PACKAGE_REPLACED));
                if (owner.equals(event.name)) send(new Intent(Intent.ACTION_MY_PACKAGE_REPLACED).setPackage(owner));
            }
        }
    }
}
