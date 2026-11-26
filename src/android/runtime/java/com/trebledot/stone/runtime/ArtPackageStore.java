package com.trebledot.stone.runtime;

import android.content.Context;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.SigningInfo;
import android.os.Build;
import android.os.Parcel;
import android.system.Os;
import android.util.Base64;
import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.TreeSet;
import java.util.UUID;

/** Durable installed-package state. Call preparation and commits on a worker thread. */
final class ArtPackageStore {
    private static final Object PROCESS_LOCK = new Object();
    private static final Object REMOVAL_LOCK = new Object();
    private static final int MAX_INDEX = 16 * 1024 * 1024;
    private final File root = new File("/data/system/stone-packages");
    private final File code = new File("/data/app/stone");
    private final ArtPackage running;
    private String cachedIdentity;
    private Properties cachedState;

    ArtPackageStore(ArtPackage running) { this.running = running; }
    private boolean isRunning(String name) { return running != null && running.applicationInfo.packageName.equals(name); }

    static final class Failure extends IOException {
        final int status;
        Failure(int status, String message) { super(message); this.status = status; }
    }

    static final class UserActionRequired extends IOException {
        UserActionRequired() { super("This update requires user confirmation"); }
    }

    static void validateLaunchSource(PackageInfo installed, String source, int targetSdk, long version) throws IOException {
        SigningInfo supplied = ArtArchive.verify(source,targetSdk);
        if (installed.signingInfo == null || !canReplace(installed.signingInfo,supplied))
            throw new IOException("Selected APK signer does not match the installed application");
        if (version > installed.getLongVersionCode())
            throw new IOException("Selected APK is newer than the installed application; install it before launching");
    }

    private static boolean automaticUpdate(Properties state, PackageInfo next, PackageInfo old, String installer, PackageInfo caller) {
        int minimum;
        switch (Build.VERSION.SDK_INT) {
            case 31: case 32: minimum = 29; break;
            case 33: minimum = 30; break;
            case 34: minimum = 31; break;
            case 35: minimum = 33; break;
            case 36: minimum = 34; break;
            case 37: minimum = 35; break;
            default: return false; // Do not guess future platform eligibility.
        }
        String updateOwner = state.getProperty("update-owner." + next.packageName);
        return old != null && caller != null && installer.equals(caller.packageName)
            && next.applicationInfo.targetSdkVersion >= minimum
            && caller.requestedPermissions != null && java.util.Arrays.asList(caller.requestedPermissions)
                .contains("android.permission.UPDATE_PACKAGES_WITHOUT_USER_ACTION")
            && (updateOwner != null ? installer.equals(updateOwner)
                : installer.equals(next.packageName) || installer.equals(state.getProperty("installer." + next.packageName)));
    }

    final class Candidate implements Closeable {
        final PackageInfo info;
        private final File directory;
        private final String[] launchers;
        private boolean committed, closed;
        private Candidate(PackageInfo info, File directory, String[] launchers) {
            this.info = info; this.directory = directory; this.launchers = launchers;
        }
        @Override public synchronized void close() throws IOException {
            if (closed || committed) return;
            closed = true;
            discard(directory);
        }
    }

    Candidate prepare(Context context, String source, String expectedPackage) throws IOException {
        return prepare(context.getPackageManager(), source, expectedPackage);
    }

    Candidate prepare(PackageManager manager, String source, String expectedPackage) throws IOException {
        if (!code.isDirectory() && !code.mkdirs()) throw new IOException("Cannot create the installed-code directory");
        File directory = new File(code, UUID.randomUUID().toString());
        if (!directory.mkdir()) throw new IOException("Cannot allocate installed-code staging");
        boolean ready = false;
        try {
            File file = new File(directory, "base.apk");
            try (FileInputStream input = new FileInputStream(source); FileOutputStream output = new FileOutputStream(file)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                output.getFD().sync();
            }
            if (!file.setReadOnly()) throw new IOException("Cannot protect installed APK code");
            // The owned copy is the exact file that will be published after user confirmation.
            PackageInfo info;
            try {
                info = ArtArchive.inspect(manager, file.getPath());
                ArtPackageCompatibility.checkNativeLibraries(file.getPath(),
                    ArtPackageCompatibility.imageAbi(Build.SUPPORTED_64_BIT_ABIS));
                if (info.sharedUserId != null)
                    throw new ArtPackageCompatibility.Incompatible("shared-uid", "", "", "Shared-UID APK installation is not supported");
                if (info.splitNames != null && info.splitNames.length != 0)
                    throw new ArtPackageCompatibility.Incompatible("split-apk", "", "", "Split APK installation is not supported; select a standalone APK");
            }
            catch (IOException error) {
                Failure invalid = new Failure(error instanceof ArtPackageCompatibility.Incompatible
                    ? PackageInstaller.STATUS_FAILURE_INCOMPATIBLE : PackageInstaller.STATUS_FAILURE_INVALID, error.getMessage());
                invalid.initCause(error);
                throw invalid;
            }
            validateName(info.packageName);
            if (expectedPackage != null && !expectedPackage.equals(info.packageName))
                throw new Failure(PackageInstaller.STATUS_FAILURE_INVALID, "APK package does not match the install session");
            Candidate candidate = new Candidate(info, directory, ArtPackage.launcherNames(info));
            ready = true;
            return candidate;
        } finally { if (!ready) discard(directory); }
    }

    PackageInfo install(Context context, String source, String installer, String expectedPackage) throws IOException {
        try (Candidate candidate = prepare(context, source, expectedPackage)) { return commit(candidate, installer); }
    }

    PackageInfo commit(Candidate candidate, String installer) throws IOException {
        return commit(candidate, installer, null);
    }

    PackageInfo commit(Candidate candidate, String installer, String transaction) throws IOException {
        return commit(candidate, installer, transaction, null);
    }

    PackageInfo commit(Candidate candidate, String installer, String transaction, PackageInfo automaticInstaller) throws IOException {
        return commit(candidate,installer,transaction,automaticInstaller,automaticInstaller != null,false);
    }

    PackageInfo commit(Candidate candidate, String installer, String transaction, PackageInfo caller, boolean automatic, boolean requestOwnership) throws IOException {
        validateName(installer);
        if (requestOwnership && (caller == null || !installer.equals(caller.packageName) || caller.requestedPermissions == null
                || !java.util.Arrays.asList(caller.requestedPermissions).contains("android.permission.ENFORCE_UPDATE_OWNERSHIP")))
            throw new SecurityException("Update ownership requires the installer's manifest permission");
        synchronized (candidate) {
            if (candidate.closed || candidate.committed) throw new IOException("Install candidate is already closed or committed");
            PackageInfo installed = locked(state -> {
                if (transaction != null && state.containsKey("receipt." + transaction))
                    throw new Failure(PackageInstaller.STATUS_FAILURE_ABORTED, "Install session was already published");
                Properties updated = (Properties) state.clone();
                PackageInfo next = copy(candidate.info);
                if (state.containsKey("removing." + next.packageName))
                    throw new Failure(PackageInstaller.STATUS_FAILURE_BLOCKED, "Package removal must finish before installing again");
                PackageInfo old = record(state, next.packageName);
                if (old == null && isRunning(next.packageName))
                    old = running.packageInfo(PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_SIGNATURES);
                if (old != null) {
                    if (old.signingInfo == null || !canReplace(next.signingInfo, old.signingInfo))
                        throw new Failure(PackageInstaller.STATUS_FAILURE_CONFLICT, "APK signer cannot replace the installed package");
                    if (next.getLongVersionCode() < old.getLongVersionCode())
                        throw new Failure(PackageInstaller.STATUS_FAILURE_CONFLICT, "APK version downgrade is not allowed");
                }
                // Check the current record under the same cross-process lock
                // as publication. Removal or installer changes cannot turn an
                // automatic update into an unconfirmed first/foreign install.
                if (automatic && !automaticUpdate(state, next, old, installer, caller))
                    throw new UserActionRequired();
                long now = System.currentTimeMillis();
                next.firstInstallTime = old == null || old.firstInstallTime == 0 ? now : old.firstInstallTime;
                next.lastUpdateTime = now;
                int nextUid = Integer.parseInt(state.getProperty("nextUid", "10001"));
                if (old == null) {
                    if (nextUid <= 10000 || nextUid > 19999) throw new IOException("Installed-package UID range exhausted or invalid");
                    next.applicationInfo.uid = nextUid;
                    updated.setProperty("nextUid", Integer.toString(nextUid + 1));
                } else next.applicationInfo.uid = old.applicationInfo.uid;
                next.applicationInfo.flags |= ApplicationInfo.FLAG_INSTALLED;
                if (next.requestedPermissionsFlags != null) for (int i = 0; i < next.requestedPermissionsFlags.length; i++)
                    next.requestedPermissionsFlags[i] &= ~PackageInfo.REQUESTED_PERMISSION_GRANTED;
                next.applicationInfo.dataDir = old == null ? "/data/data/" + next.packageName : old.applicationInfo.dataDir;
                next.applicationInfo.deviceProtectedDataDir = old == null
                    ? next.applicationInfo.dataDir + "/device-protected" : old.applicationInfo.deviceProtectedDataDir;
                for (String path : new String[]{next.applicationInfo.dataDir, next.applicationInfo.deviceProtectedDataDir}) {
                    File data = new File(path);
                    if (!data.isDirectory() && !data.mkdirs()) throw new IOException("Cannot prepare installed-package data directory");
                }
                for (android.content.pm.ComponentInfo[] components : new android.content.pm.ComponentInfo[][]{
                        next.activities, next.receivers, next.services, next.providers}) {
                    if (components != null) for (android.content.pm.ComponentInfo component : components) component.applicationInfo = next.applicationInfo;
                }
                try {
                    ApplicationInfo.class.getField("credentialProtectedDataDir").set(next.applicationInfo, next.applicationInfo.dataDir);
                } catch (ReflectiveOperationException error) { throw new IOException("Unsupported image ApplicationInfo", error); }
                updated.setProperty("package." + next.packageName, encode(next));
                updated.setProperty("capabilities." + next.packageName, capabilities(next.signingInfo));
                updated.setProperty("installer." + next.packageName, installer);
                if (old == null && requestOwnership) updated.setProperty("update-owner." + next.packageName,installer);
                else if (!installer.equals(state.getProperty("update-owner." + next.packageName)))
                    updated.remove("update-owner." + next.packageName);
                updated.setProperty("launchers." + next.packageName, String.join("\n", candidate.launchers));
                updated.setProperty("code-owner." + candidate.directory.getName(), next.packageName);
                if (old != null && old.applicationInfo.sourceDir != null) {
                    File previous = new File(old.applicationInfo.sourceDir).getCanonicalFile().getParentFile();
                    if (previous != null && code.getCanonicalFile().equals(previous.getParentFile()))
                        updated.setProperty("code-owner." + previous.getName(), next.packageName);
                }
                if (transaction != null) updated.setProperty("receipt." + transaction, next.packageName);
                ArtPackageEvents.append(updated, old == null ? "add" : "replace", next.packageName, next.applicationInfo.uid);
                save(updated);
                candidate.committed = true;
                return copy(next);
            });
            ArtShell.installed(installed, candidate.launchers.length != 0);
            return installed;
        }
    }

    String receipt(String transaction) throws IOException {
        return locked(state -> state.getProperty("receipt." + transaction));
    }

    ArtPackageEvents.Snapshot changes(ArtPackageEvents.Snapshot previous) throws IOException {
        return locked(state -> previous != null && java.util.Objects.equals(previous.identity, cachedIdentity)
            ? previous : new ArtPackageEvents.Snapshot(cachedIdentity, state));
    }

    static PackageInfo changeInfo(String value, String name) throws IOException {
        validateName(name);
        PackageInfo info = decode(value);
        if (!name.equals(info.packageName) || info.applicationInfo == null || info.applicationInfo.uid < 10000 || info.applicationInfo.uid > 19999)
            throw new IOException("Invalid package change identity");
        String path = info.applicationInfo.sourceDir;
        File source = path == null ? null : new File(path);
        if (source == null || source.getParentFile() == null || !source.getParentFile().getName().matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
                || !("/data/app/stone/" + source.getParentFile().getName() + "/base.apk").equals(path))
            throw new IOException("Invalid historical package code path");
        return info;
    }

    PackageInfo get(String name, long flags) throws IOException {
        if (name == null || !name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+")) return null;
        return locked(state -> {
            PackageInfo info = record(state, name);
            if (info == null && isRunning(name)) return running.packageInfo(flags);
            return info == null ? null : filter(info, flags);
        });
    }

    static PackageInfo registered(String name) throws IOException {
        validateName(name);
        ArtPackageStore store = new ArtPackageStore(null);
        return store.locked(state -> store.record(state, name));
    }

    /** One-time migration before Application.onCreate; normal queries never reopen APKs. */
    void initializeLaunchers() throws IOException {
        List<PackageInfo> missing = locked(state -> {
            List<PackageInfo> result = new ArrayList<>();
            for (String name : names(state)) if (!state.containsKey("launchers." + name)) result.add(record(state, name));
            return result;
        });
        for (PackageInfo old : missing) {
            String value = String.join("\n", ArtPackage.launcherNames(old));
            locked(state -> {
                PackageInfo current = record(state, old.packageName);
                if (current != null && current.applicationInfo.sourceDir.equals(old.applicationInfo.sourceDir)
                        && !state.containsKey("launchers." + old.packageName)) {
                    Properties updated = (Properties) state.clone();
                    updated.setProperty("launchers." + old.packageName, value);
                    save(updated);
                }
                return null;
            });
        }
        ArtPackageCatalog.request(this);
    }

    /** Read-only UI summary. The registry remains the authority for every operation. */
    boolean exportCatalog() throws IOException {
        final String[] identity = new String[1];
        Properties snapshot = locked(state -> {
            identity[0] = cachedIdentity;
            return (Properties) state.clone();
        });
        try {
            org.json.JSONObject catalog = new org.json.JSONObject().put("format", 1)
                .put("packageManagementAvailable", ArtSettingsConfirmation.enabled());
            org.json.JSONArray apps = new org.json.JSONArray();
            for (String name : names(snapshot)) {
                PackageInfo info;
                try { info = record(snapshot, name); }
                catch (IOException error) {
                    if (locked(state -> !java.util.Objects.equals(identity[0], cachedIdentity))) {
                        ArtPackageCatalog.request(this); return false;
                    }
                    throw error;
                }
                String label = info.packageName;
                try { label = ArtArchive.labelInLocale(info, java.util.Locale.getDefault().toLanguageTag()); }
                catch (IOException error) { android.util.Log.w("StonePackages", "Cannot read settings label: " + info.packageName, error); }
                publishSettingsIcon(info);
                apps.put(new org.json.JSONObject().put("packageName", info.packageName).put("name", label)
                    .put("version", info.versionName == null ? Long.toString(info.getLongVersionCode()) : info.versionName)
                    .put("uid", info.applicationInfo.uid).put("installer", snapshot.getProperty("installer." + info.packageName))
                    .put("requestedPermissions",new org.json.JSONArray(info.requestedPermissions==null ? java.util.Collections.emptyList() : java.util.Arrays.asList(info.requestedPermissions)))
                    .put("storageIdentity",ArtStorage.identity(info))
                    .put("codeBytes", new File(info.applicationInfo.sourceDir).length()));
            }
            byte[] bytes = catalog.put("apps", apps).toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
            return locked(state -> {
                // Parsing labels happens outside the registry lock. Never publish an old
                // snapshot over a concurrent install, upgrade or package removal.
                if (!java.util.Objects.equals(identity[0], cachedIdentity)) { ArtPackageCatalog.request(this); return false; }
                File pending = new File(root, "packages.json.tmp");
                try {
                    try (FileOutputStream output = new FileOutputStream(pending)) { output.write(bytes); output.getFD().sync(); }
                    Os.rename(pending.getPath(), new File(root, "packages.json").getPath());
                } catch (android.system.ErrnoException error) { throw new IOException("Cannot publish the settings application catalog", error); }
                finally { pending.delete(); }
                return true;
            });
        } catch (org.json.JSONException error) { throw new IOException("Cannot encode the settings application catalog", error); }
    }

    private void publishSettingsIcon(PackageInfo info) {
        File directory = new File(root, "icons");
        File target = new File(directory, info.packageName + ".png");
        File pending = null;
        try {
            byte[] png = ArtAppearance.settingsIcon(info);
            if (png.length == 0) { target.delete(); return; }
            if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create settings icon cache");
            pending = File.createTempFile("icon-", ".tmp", directory);
            try (FileOutputStream output = new FileOutputStream(pending)) { output.write(png); }
            Os.rename(pending.getPath(), target.getPath());
        } catch (Exception error) {
            android.util.Log.w("StonePackages", "Cannot publish settings icon: " + info.packageName, error);
        } finally { if (pending != null) pending.delete(); }
    }

    private ActivityInfo activity(PackageInfo info, String name, long flags) throws IOException {
        if (info == null || info.activities == null) return null;
        java.util.Map<String, android.content.pm.ComponentInfo> components = new java.util.HashMap<>();
        for (ActivityInfo entry : info.activities) components.put(entry.name, entry);
        ArtComponentSettings settings = new ArtComponentSettings(info.packageName, new File(info.applicationInfo.dataDir), components);
        for (ActivityInfo entry : info.activities) if (entry.name.equals(name)) {
            boolean enabled = info.applicationInfo.enabled && settings.enabled(entry);
            if (entry.targetActivity != null) {
                android.content.pm.ComponentInfo target = components.get(entry.targetActivity);
                enabled &= target != null && settings.enabled(target);
            }
            if (!enabled && (flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0) return null;
            ActivityInfo result = new ActivityInfo(entry);
            result.enabled = enabled;
            result.applicationInfo = new ApplicationInfo(info.applicationInfo);
            if ((flags & PackageManager.GET_META_DATA) == 0) result.metaData = null;
            return result;
        }
        return null;
    }

    ActivityInfo activityInfo(ComponentName component, long flags) throws IOException {
        if (component == null) return null;
        PackageInfo info = get(component.getPackageName(), PackageManager.GET_ACTIVITIES | (flags & PackageManager.GET_META_DATA));
        return activity(info, component.getClassName(), flags);
    }

    List<ResolveInfo> queryActivities(Intent intent, long flags) throws IOException {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent == null || intent.getSelector() != null) return result;
        ComponentName explicit = intent.getComponent();
        if (explicit != null) {
            if (intent.getPackage() != null && !intent.getPackage().equals(explicit.getPackageName())) return result;
            ActivityInfo info = activityInfo(explicit, flags);
            if (info != null && info.exported) { ResolveInfo match = new ResolveInfo(); match.activityInfo = info; result.add(match); }
            return result;
        }
        // Copy identities under the registry lock; component-state I/O runs outside it.
        List<PackageInfo> records = new ArrayList<>();
        locked(state -> {
            for (String name : names(state)) if ((intent.getPackage() == null || name.equals(intent.getPackage()))
                    && !isRunning(name)) {
                records.add(record(state, name));
            }
            return null;
        });
        for (PackageInfo record : records) for (ArtPackage.FilterMatch match : ArtPackage.installedMatches(record,intent,flags)) {
            ActivityInfo info = activity(record,match.name,flags);
            if (info != null && info.exported) result.add(match.resolve(info,flags));
        }
        return result;
    }

    List<PackageInfo> all(long flags) throws IOException {
        return locked(state -> {
            TreeSet<String> names = names(state);
            if (running != null) names.add(running.applicationInfo.packageName);
            List<PackageInfo> result = new ArrayList<>();
            for (String name : names) {
                PackageInfo info = record(state, name);
                result.add(info == null ? running.packageInfo(flags) : filter(info, flags));
            }
            return result;
        });
    }

    String installer(String name) throws IOException {
        if (name == null || !name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+")) return null;
        return locked(state -> state.getProperty("installer." + name));
    }

    String[] installSource(String name) throws IOException {
        if (name == null || !name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+")) return null;
        return locked(state -> !state.containsKey("package." + name) && !isRunning(name) ? null
            : new String[]{state.getProperty("installer." + name),state.getProperty("update-owner." + name)});
    }

    private static void forgetInstaller(Properties state, String removed) {
        state.remove("update-owner." + removed);
        for (String key : state.stringPropertyNames())
            if ((key.startsWith("installer.") || key.startsWith("update-owner.")) && removed.equals(state.getProperty(key))) state.remove(key);
    }

    // Removal of the registration only. The uninstall flow must separately handle user data.
    void remove(String name) throws IOException {
        validateName(name);
        locked(state -> {
            Properties updated = (Properties) state.clone();
            updated.remove("package." + name);
            updated.remove("installer." + name);
            forgetInstaller(updated,name);
            updated.remove("capabilities." + name);
            updated.remove("launchers." + name);
            save(updated);
            return null;
        });
    }

    PackageInfo removalInfo(String name) throws IOException {
        validateName(name);
        return locked(state -> {
            PackageInfo info = record(state, name);
            if (info == null && state.containsKey("removing." + name)) info = decode(state.getProperty("removing." + name));
            return info == null ? null : copy(info);
        });
    }

    List<PackageInfo> pendingRemovals() throws IOException {
        return locked(state -> {
            List<PackageInfo> result = new ArrayList<>();
            for (String key : state.stringPropertyNames()) if (key.startsWith("removing.")) {
                String name = key.substring("removing.".length()); validateName(name);
                PackageInfo info = decode(state.getProperty(key));
                if (!name.equals(info.packageName) || info.applicationInfo == null || info.applicationInfo.sourceDir == null)
                    throw new IOException("Invalid pending removal identity");
                result.add(info);
            }
            return result;
        });
    }

    private static RandomAccessFile executionLock(File path) throws IOException {
        try { return new RandomAccessFile(path, "rw"); }
        catch (IOException error) {
            Failure blocked = new Failure(PackageInstaller.STATUS_FAILURE_BLOCKED,
                ArtText.get(ArtStrings.package_busy));
            blocked.initCause(error); throw blocked;
        }
    }

    /** Atomically remove visibility, then clean owned paths without holding the query lock. */
    PackageInfo uninstall(String name, String expectedSource) throws IOException {
        validateName(name);
        if (isRunning(name))
            throw new Failure(PackageInstaller.STATUS_FAILURE_BLOCKED, "The running application cannot be removed by its own host");
        synchronized (REMOVAL_LOCK) {
            if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create the package registry");
            File appLocks = new File(root, "app-locks");
            if (!appLocks.isDirectory() && !appLocks.mkdirs()) throw new IOException("Cannot create application locks");
            File dataDirectory = new File("/data/data/" + name);
            if (!dataDirectory.getAbsoluteFile().equals(dataDirectory.getCanonicalFile()))
                throw new IOException("Uninstall data path is not an owned application directory");
            // Native APK hosts deny write sharing on these files. Opening them
            // for write excludes launches until cleanup finishes, before visibility changes.
            try (RandomAccessFile execution = executionLock(new File(appLocks, name + ".lock"));
                    RandomAccessFile legacy = dataDirectory.isDirectory()
                        ? executionLock(new File(dataDirectory, ".stone-app.lock")) : null;
                    RandomAccessFile file = new RandomAccessFile(new File(root, "remove-" + name + ".lock"), "rw");
                    FileLock removalLock = file.getChannel().lock()) {
                List<String> directories = new ArrayList<>();
                PackageInfo removed = markRemoval(name, expectedSource, directories);
                // Keep the durable removal marker and app launch locks until
                // Service has cleared every image's queued/persisted jobs.
                ArtJobs.packageRemoved(name);
                deleteOwnedTree(new File("/data/data/" + name));
                for (String directory : directories) deleteOwnedTree(new File(code, directory));
                ArtNotifications.forget(name);
                ArtStorage.forget(name);
                ArtShell.removed(name);
                new File(new File(root, "icons"), name + ".png").delete();
                locked(state -> {
                    Properties updated = (Properties) state.clone();
                    updated.remove("removing." + name);
                    for (String directory : directories) updated.remove("code-owner." + directory);
                    ArtPackageEvents.append(updated, "remove", name, removed.applicationInfo.uid);
                    save(updated);
                    return null;
                });
                return removed;
            }
        }
    }

    // Caller holds the per-package removal lock across this durable phase and cleanup.
    private PackageInfo markRemoval(String name, String expectedSource, List<String> directories) throws IOException {
        validateName(name);
        return locked(state -> {
            PackageInfo info = state.containsKey("removing." + name)
                ? decode(state.getProperty("removing." + name)) : record(state, name);
            if (info == null) throw new Failure(PackageInstaller.STATUS_FAILURE_INVALID, "Package is not installed");
            if (!name.equals(info.packageName) || info.applicationInfo == null || info.applicationInfo.sourceDir == null
                    || !info.applicationInfo.sourceDir.equals(expectedSource))
                throw new Failure(PackageInstaller.STATUS_FAILURE_CONFLICT, "Package changed after uninstall confirmation");
            String data = "/data/data/" + name;
            if (!data.equals(info.applicationInfo.dataDir)
                    || !new File(data).getAbsoluteFile().equals(new File(data).getCanonicalFile()))
                throw new IOException("Uninstall data path is not an owned application directory");
            File current = new File(expectedSource).getAbsoluteFile();
            if (!"base.apk".equals(current.getName()) || current.getParentFile() == null
                    || !code.getCanonicalFile().equals(current.getParentFile().getParentFile()))
                throw new IOException("Uninstall code path is outside the managed directory");
            Properties updated = (Properties) state.clone();
            updated.setProperty("code-owner." + current.getParentFile().getName(), name);
            for (String key : updated.stringPropertyNames()) if (key.startsWith("code-owner.") && name.equals(updated.getProperty(key))) {
                String directory = key.substring("code-owner.".length());
                if (!directory.matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"))
                    throw new IOException("Invalid managed-code directory identity");
                directories.add(directory);
            }
            if (!state.containsKey("removing." + name)) {
                updated.remove("package." + name);
                updated.remove("installer." + name);
                forgetInstaller(updated,name);
                updated.remove("capabilities." + name);
                updated.remove("launchers." + name);
                updated.setProperty("removing." + name, encode(info));
                save(updated);
            }
            return info;
        });
    }

    private static void deleteOwnedTree(File directory) throws IOException {
        File boundary = directory.getAbsoluteFile();
        java.util.ArrayDeque<File> pending = new java.util.ArrayDeque<>(), directories = new java.util.ArrayDeque<>();
        pending.push(boundary);
        while (!pending.isEmpty()) {
            File file = pending.pop();
            android.system.StructStat stat;
            try { stat = Os.lstat(file.getPath()); }
            catch (android.system.ErrnoException error) {
                if (error.errno == android.system.OsConstants.ENOENT) continue;
                throw new IOException("Cannot inspect uninstall path", error);
            }
            if (android.system.OsConstants.S_ISLNK(stat.st_mode)) {
                if (!file.delete()) throw new IOException("Cannot unlink application data link");
                continue;
            }
            File canonical = file.getCanonicalFile();
            if (!canonical.equals(boundary) && !canonical.getPath().startsWith(boundary.getPath() + "/"))
                throw new IOException("Uninstall path escapes its owned directory");
            if (android.system.OsConstants.S_ISDIR(stat.st_mode)) {
                File[] entries = file.listFiles();
                if (entries == null) throw new IOException("Cannot list application data for removal");
                directories.push(file);
                for (File child : entries) pending.push(child);
            } else {
                if (!file.canWrite() && !file.setWritable(true)) throw new IOException("Cannot make owned uninstall file writable");
                if (!file.delete()) throw new IOException("Cannot delete owned application file");
            }
        }
        while (!directories.isEmpty()) if (!directories.pop().delete()) throw new IOException("Cannot delete owned application directory");
    }

    private static boolean canReplace(SigningInfo next, SigningInfo previous) throws IOException {
        try {
            Class<?> details = Class.forName("android.content.pm.SigningDetails");
            Object newer = SigningInfo.class.getMethod("getSigningDetails").invoke(next);
            Object older = SigningInfo.class.getMethod("getSigningDetails").invoke(previous);
            return (Boolean) details.getMethod("checkCapability", details, int.class).invoke(newer, older, 1); // INSTALLED_DATA
        } catch (ReflectiveOperationException error) { throw new IOException("Cannot verify package replacement signing capability", error); }
    }

    private interface Work<T> { T run(Properties state) throws IOException; }
    private <T> T locked(Work<T> work) throws IOException {
        synchronized (PROCESS_LOCK) {
            if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create the package registry");
            try (RandomAccessFile file = new RandomAccessFile(new File(root, "registry.lock"), "rw");
                    FileLock lock = file.getChannel().lock()) {
                return work.run(load());
            }
        }
    }

    private Properties load() throws IOException {
        Properties state = new Properties();
        File file = new File(root, "packages.properties");
        android.system.StructStat stat;
        try { stat = Os.stat(file.getPath()); }
        catch (android.system.ErrnoException error) {
            if (error.errno != android.system.OsConstants.ENOENT) throw new IOException("Cannot inspect package registry", error);
            cachedIdentity = null; cachedState = null; return state;
        }
        if (android.system.OsConstants.S_ISREG(stat.st_mode)) {
            String identity = stat.st_dev + ":" + stat.st_ino + ":" + stat.st_size + ":" + stat.st_mtim.tv_sec + ":" + stat.st_mtim.tv_nsec;
            if (identity.equals(cachedIdentity)) return cachedState;
            if (stat.st_size > MAX_INDEX) throw new IOException("Package registry is too large");
            try (FileInputStream input = new FileInputStream(file)) { state.load(input); }
            if (!"1".equals(state.getProperty("format")) || !Integer.toString(Build.VERSION.SDK_INT).equals(state.getProperty("api"))
                    || !Build.FINGERPRINT.equals(state.getProperty("image")))
                throw new IOException("Package registry requires metadata migration for this system image");
            try { if (Integer.parseInt(state.getProperty("nextUid")) <= 10000) throw new NumberFormatException(); }
            catch (RuntimeException error) { throw new IOException("Invalid package registry UID counter", error); }
            cachedState = state;
            cachedIdentity = identity;
        } else throw new IOException("Package registry is not a regular file");
        return state;
    }

    private void save(Properties state) throws IOException {
        state.setProperty("format", "1");
        state.setProperty("api", Integer.toString(Build.VERSION.SDK_INT));
        state.setProperty("image", Build.FINGERPRINT);
        if (!state.containsKey("nextUid")) state.setProperty("nextUid", "10001");
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        state.store(bytes, "Stone installed packages");
        if (bytes.size() > MAX_INDEX) throw new IOException("Package registry is too large");
        File pending = new File(root, "packages.properties.tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(pending)) { bytes.writeTo(output); output.getFD().sync(); }
            Os.rename(pending.getPath(), new File(root, "packages.properties").getPath());
            cachedIdentity = null;
            cachedState = null;
            ArtPackageCatalog.request(this);
            try { PackageManager.class.getMethod("invalidatePackageInfoCache").invoke(null); }
            catch (ReflectiveOperationException error) { android.util.Log.w("StonePackages", "Could not invalidate package information cache", error); }
            try { Class.forName("android.app.ApplicationPackageManager").getMethod("invalidateGetPackagesForUidCache").invoke(null); }
            catch (ReflectiveOperationException error) { android.util.Log.w("StonePackages", "Could not invalidate package UID cache", error); }
        } catch (android.system.ErrnoException error) { throw new IOException("Could not publish package registry", error); }
        finally { pending.delete(); }
    }

    private PackageInfo record(Properties state, String name) throws IOException {
        String encoded = state.getProperty("package." + name);
        if (encoded == null) return null;
        PackageInfo info = decode(encoded);
        if (!name.equals(info.packageName) || info.applicationInfo == null || info.signingInfo == null
                || info.applicationInfo.uid <= 0) throw new IOException("Invalid installed-package identity: " + name);
        restoreCapabilities(info.signingInfo, state.getProperty("capabilities." + name, ""));
        if (info.applicationInfo.sourceDir == null) throw new IOException("Installed APK path is missing: " + name);
        File source = new File(info.applicationInfo.sourceDir).getCanonicalFile();
        if (source.getParentFile() == null || !code.getCanonicalFile().equals(source.getParentFile().getParentFile())
                || !source.getName().equals("base.apk") || !source.isFile())
            throw new IOException("Installed APK is missing or outside the code directory: " + name);
        return info;
    }

    private static PackageInfo decode(String encoded) throws IOException {
        Parcel parcel = Parcel.obtain();
        try {
            byte[] bytes = Base64.decode(encoded, Base64.DEFAULT);
            parcel.unmarshall(bytes, 0, bytes.length);
            parcel.setDataPosition(0);
            return PackageInfo.CREATOR.createFromParcel(parcel);
        } catch (RuntimeException error) { throw new IOException("Invalid package registry record", error); }
        finally { parcel.recycle(); }
    }

    private static TreeSet<String> names(Properties state) throws IOException {
        TreeSet<String> result = new TreeSet<>();
        for (String key : state.stringPropertyNames()) if (key.startsWith("package.")) {
            String name = key.substring(8);
            validateName(name);
            result.add(name);
        }
        return result;
    }

    private static String encode(PackageInfo info) {
        Parcel parcel = Parcel.obtain();
        try { info.writeToParcel(parcel, 0); return Base64.encodeToString(parcel.marshall(), Base64.NO_WRAP); }
        finally { parcel.recycle(); }
    }

    private static PackageInfo copy(PackageInfo info) {
        Parcel parcel = Parcel.obtain();
        try {
            info.writeToParcel(parcel, 0); parcel.setDataPosition(0);
            PackageInfo result = PackageInfo.CREATOR.createFromParcel(parcel);
            if (info.signingInfo != null) restoreCapabilities(result.signingInfo, capabilities(info.signingInfo));
            return result;
        }
        catch (IOException error) { throw new IllegalStateException("Cannot copy package signing capabilities", error); }
        finally { parcel.recycle(); }
    }

    // Android Signature's Parcel contains the certificate bytes, but not rotation
    // capability flags. Preserve those separately before using SigningDetails policy.
    private static String capabilities(SigningInfo signing) throws IOException {
        if (!signing.hasPastSigningCertificates()) return "";
        try {
            StringBuilder result = new StringBuilder();
            for (android.content.pm.Signature certificate : signing.getSigningCertificateHistory()) {
                if (result.length() > 0) result.append(',');
                result.append(android.content.pm.Signature.class.getMethod("getFlags").invoke(certificate));
            }
            return result.toString();
        } catch (ReflectiveOperationException error) { throw new IOException("Cannot read package signing capabilities", error); }
    }

    private static void restoreCapabilities(SigningInfo signing, String encoded) throws IOException {
        if (!signing.hasPastSigningCertificates()) {
            if (!encoded.isEmpty()) throw new IOException("Unexpected signing capabilities");
            return;
        }
        String[] flags = encoded.split(",");
        android.content.pm.Signature[] certificates = signing.getSigningCertificateHistory();
        if (flags.length != certificates.length) throw new IOException("Missing signing rotation capabilities");
        try {
            for (int i = 0; i < flags.length; i++)
                android.content.pm.Signature.class.getMethod("setFlags", int.class).invoke(certificates[i], Integer.parseInt(flags[i]));
        } catch (ReflectiveOperationException | NumberFormatException error) { throw new IOException("Invalid signing rotation capabilities", error); }
    }

    private static PackageInfo filter(PackageInfo info, long flags) {
        if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) == 0) info.signingInfo = null;
        if ((flags & PackageManager.GET_SIGNATURES) == 0) info.signatures = null;
        if ((flags & PackageManager.GET_ACTIVITIES) == 0) info.activities = null;
        if ((flags & PackageManager.GET_RECEIVERS) == 0) info.receivers = null;
        if ((flags & PackageManager.GET_SERVICES) == 0) info.services = null;
        if ((flags & PackageManager.GET_PROVIDERS) == 0) info.providers = null;
        if ((flags & PackageManager.GET_PERMISSIONS) == 0) { info.permissions = null; info.requestedPermissions = null; info.requestedPermissionsFlags = null; }
        return info;
    }

    static void validateName(String name) throws IOException {
        if (name == null || !name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"))
            throw new IOException("Invalid package name");
    }

    private void discard(File directory) throws IOException {
        if (!code.getCanonicalFile().equals(directory.getCanonicalFile().getParentFile())) throw new IOException("Invalid code staging directory");
        File file = new File(directory, "base.apk");
        if (file.exists()) {
            if (!file.setWritable(true)) throw new IOException("Cannot make staged APK code writable for cleanup");
            if (!file.delete()) throw new IOException("Cannot delete staged APK code");
        }
        if (directory.exists() && !directory.delete()) throw new IOException("Cannot discard code staging directory");
    }
}
