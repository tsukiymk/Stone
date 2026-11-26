package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageInstaller.SessionInfo;
import android.content.pm.PackageInstaller.SessionParams;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Build;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.util.Base64;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;

/** Persistent install staging backed by the selected image's PackageInstaller and FileBridge. */
final class ArtPackageInstaller {
    private static final String SESSION = "android.content.pm.IPackageInstallerSession";
    private static final int MAX_METADATA = 4 * 1024 * 1024;
    private final File root = new File("/data/system/stone-package-installer");
    private final ArtPackage apk;
    private final int user;
    private final ArtPackageStore packages;
    private volatile Context context;
    private static final String CONFIRM = "com.trebledot.stone.action.CONFIRM_INSTALL";
    private static final String NONCE = "com.trebledot.stone.install.NONCE";
    private final SecureRandom random = new SecureRandom();
    private final Map<Integer, Record> live = new HashMap<>();
    private final Map<IBinder, IInterface> callbacks = new LinkedHashMap<>();

    ArtPackageInstaller(ArtPackage apk, int user, ArtPackageStore packages) { this.apk = apk; this.user = user; this.packages = packages; }
    void attach(Context context) { this.context = context; }
    boolean canRequestInstalls() {
        return apk.applicationInfo.targetSdkVersion < 26
            || java.util.Arrays.asList(apk.packageInfo.requestedPermissions).contains("android.permission.REQUEST_INSTALL_PACKAGES");
    }

    synchronized Object call(Object proxy, Method method, Object[] args) throws Throwable {
        try {
            switch (method.getName()) {
                case "createSession":
                    checkOwner((String) args[1], (Integer) args[3]);
                    return create((SessionParams) args[0], (String) args[2]);
                case "openSession": {
                    Record record = owned((Integer) args[0]);
                    IInterface handle = ArtServices.service(SESSION, new Handle(record)::call).queryLocalInterface(SESSION);
                    if (++record.opens == 1) notifyCallbacks("onSessionActiveChanged", record.id, true);
                    return handle;
                }
                case "getSessionInfo": {
                    Record record = find((Integer) args[0]);
                    return record == null || record.abandoned || record.user != user ? null : info(record);
                }
                case "getMySessions":
                    checkOwner((String) args[0], (Integer) args[1]);
                    return method.getReturnType().getConstructor(List.class).newInstance(list(true));
                case "getAllSessions":
                    checkUser((Integer) args[0]);
                    return method.getReturnType().getConstructor(List.class).newInstance(list(false));
                case "updateSessionAppIcon": {
                    Record record = owned((Integer) args[0]);
                    mutable(record);
                    byte[] icon = encodeIcon((Bitmap) args[1]);
                    byte[] previous = record.icon;
                    record.icon = icon;
                    try { persist(record); }
                    catch (Exception error) { record.icon = previous; throw error; }
                    notifyCallbacks("onSessionBadgingChanged", record.id);
                    return null;
                }
                case "updateSessionAppLabel": {
                    Record record = owned((Integer) args[0]);
                    mutable(record);
                    Object previous = get(record.params, "appLabel");
                    set(record.params, "appLabel", args[1]);
                    try { persist(record); }
                    catch (Exception error) { set(record.params, "appLabel", previous); throw error; }
                    notifyCallbacks("onSessionBadgingChanged", record.id);
                    return null;
                }
                case "abandonSession": abandon(owned((Integer) args[0])); return null;
                case "setPermissionsResult": throw new SecurityException("Only Stone's confirmation Activity can approve an install");
                case "registerCallback":
                    checkUser((Integer) args[1]);
                    IInterface callback = (IInterface) args[0];
                    callbacks.put(callback.asBinder(), callback);
                    return null;
                case "unregisterCallback": callbacks.remove(((IInterface) args[0]).asBinder()); return null;
                default: throw new UnsupportedOperationException("ART package installer: " + method.getName());
            }
        } catch (IOException error) { throw transport(error); }
        catch (android.system.ErrnoException error) { throw transport(new IOException(error)); }
    }

    private void checkOwner(String owner, int requestedUser) {
        checkUser(requestedUser);
        if (!apk.applicationInfo.packageName.equals(owner)) throw new SecurityException("Installer package does not match the current application");
    }

    private void checkUser(int requested) {
        if (requested != user) throw new SecurityException("Install sessions are restricted to the current user");
    }

    private Record owned(int id) throws Exception {
        Record record = find(id);
        if (record == null || record.abandoned || record.user != user || !apk.applicationInfo.packageName.equals(record.owner))
            throw new SecurityException("Unknown or unowned install session: " + id);
        live.put(id, record);
        return record;
    }

    private int create(SessionParams params, String attribution) throws Exception {
        if (!canRequestInstalls()) throw new SecurityException("Installer must declare REQUEST_INSTALL_PACKAGES");
        if (params == null) throw new IllegalArgumentException("Missing session parameters");
        int mode = (Integer) get(params, "mode");
        if (mode != SessionParams.MODE_FULL_INSTALL && mode != SessionParams.MODE_INHERIT_EXISTING)
            throw new IllegalArgumentException("Invalid install mode: " + mode);
        if ((Boolean) get(params, "isStaged") || (Boolean) get(params, "isMultiPackage") || get(params, "dataLoaderParams") != null)
            throw new UnsupportedOperationException("Staged, multi-package and data-loader install sessions are not connected yet");
        SessionParams copy = (SessionParams) SessionParams.class.getMethod("copy").invoke(params);
        int userAction = (Integer) get(copy, "requireUserAction");
        if (userAction != SessionParams.USER_ACTION_UNSPECIFIED && userAction != SessionParams.USER_ACTION_REQUIRED
                && userAction != SessionParams.USER_ACTION_NOT_REQUIRED) throw new IllegalArgumentException("Invalid install user-action policy");
        if (((Integer) get(copy,"installFlags") & (1 << 25) /* INSTALL_REQUEST_UPDATE_OWNERSHIP */) != 0
                && !java.util.Arrays.asList(apk.packageInfo.requestedPermissions).contains("android.permission.ENFORCE_UPDATE_OWNERSHIP"))
            throw new SecurityException("Update ownership requires declared ENFORCE_UPDATE_OWNERSHIP permission");
        byte[] icon = encodeIcon((Bitmap) get(copy, "appIcon"));
        set(copy, "appIcon", null);
        copy = decodeParams(encodeParams(copy));
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create the install-session directory");
        for (int attempt = 0; attempt < 64; attempt++) {
            int id = random.nextInt(Integer.MAX_VALUE - 1) + 1;
            File directory = directory(id);
            if (!directory.mkdir()) {
                if (!directory.exists()) throw new IOException("Cannot allocate an install session");
                continue;
            }
            Record record = new Record(id, user, apk.applicationInfo.uid, apk.applicationInfo.packageName, attribution, copy, icon);
            try { persist(record); }
            catch (Exception error) { directory.delete(); throw error; }
            live.put(id, record);
            notifyCallbacks("onSessionCreated", id);
            return id;
        }
        throw new IOException("Could not allocate a unique install session");
    }

    private List<SessionInfo> list(boolean mine) throws Exception {
        List<SessionInfo> result = new ArrayList<>();
        File[] directories = root.listFiles();
        if (directories == null) return result;
        for (File directory : directories) {
            if (!directory.getName().matches("session-[1-9][0-9]{0,9}")) continue;
            Record record;
            try { record = find(Integer.parseInt(directory.getName().substring(8))); }
            catch (IOException | RuntimeException error) { Log.w("StoneInstaller", "Ignoring invalid install session", error); continue; }
            if (record != null && !record.abandoned && record.user == user && (!mine || record.owner.equals(apk.applicationInfo.packageName)))
                result.add(info(record));
        }
        result.sort((left, right) -> Integer.compare(left.getSessionId(), right.getSessionId()));
        return result;
    }

    private File directory(int id) throws IOException {
        if (id <= 0) throw new IllegalArgumentException("Invalid session ID");
        File directory = new File(root, "session-" + id).getCanonicalFile();
        if (!root.getCanonicalFile().equals(directory.getParentFile())) throw new SecurityException("Install session escaped its directory");
        return directory;
    }

    private Record find(int id) throws Exception {
        if (id <= 0) return null;
        Record record = live.get(id);
        if (record != null) return record;
        File file = new File(directory(id), "session.properties");
        if (!file.isFile()) return null;
        if (file.length() > MAX_METADATA) throw new IOException("Install session metadata is too large");
        Properties state = new Properties();
        try (FileInputStream input = new FileInputStream(file)) { state.load(input); }
        if (!"1".equals(state.getProperty("format")) || !Integer.toString(Build.VERSION.SDK_INT).equals(state.getProperty("api")))
            throw new IOException("Unsupported install-session metadata version");
        try {
            String owner = state.getProperty("owner", "");
            if (!owner.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"))
                throw new IOException("Invalid install-session owner");
            boolean abandoned = Boolean.parseBoolean(state.getProperty("abandoned"));
            record = new Record(id, Integer.parseInt(state.getProperty("user")), Integer.parseInt(state.getProperty("uid")), owner, state.getProperty("attribution"),
                abandoned ? new SessionParams(SessionParams.MODE_FULL_INSTALL) : decodeParams(Base64.decode(state.getProperty("params"), Base64.DEFAULT)),
                abandoned ? new byte[0] : Base64.decode(state.getProperty("icon", ""), Base64.DEFAULT));
            record.created = Long.parseLong(state.getProperty("created"));
            record.updated = Long.parseLong(state.getProperty("updated"));
            record.progress = Float.parseFloat(state.getProperty("progress"));
            record.abandoned = abandoned;
            record.sealed = Boolean.parseBoolean(state.getProperty("sealed", "false"));
            record.preRequested = Boolean.parseBoolean(state.getProperty("preRequested", "false"));
            record.preApproved = Boolean.parseBoolean(state.getProperty("preApproved", "false"));
            if (record.preRequested && !record.abandoned) {
                record.prePackage = state.getProperty("prePackage");
                record.preLabel = state.getProperty("preLabel");
                record.preLocale = state.getProperty("preLocale");
                validatePreapproval(record.prePackage, record.preLabel, record.preLocale);
            } else if (record.preApproved && !record.abandoned) throw new IOException("Invalid install preapproval state");
            if (record.user < 0 || record.uid < 0 || !Float.isFinite(record.progress) || record.progress < 0 || record.progress > 1)
                throw new IOException("Invalid install-session metadata");
            return record;
        } catch (RuntimeException error) { throw new IOException("Invalid install-session metadata", error); }
    }

    private void persist(Record record) throws Exception {
        Properties state = new Properties();
        state.setProperty("format", "1");
        state.setProperty("api", Integer.toString(Build.VERSION.SDK_INT));
        state.setProperty("user", Integer.toString(record.user));
        state.setProperty("uid", Integer.toString(record.uid));
        state.setProperty("owner", record.owner);
        if (record.attribution != null) state.setProperty("attribution", record.attribution);
        state.setProperty("created", Long.toString(record.created));
        long updated = System.currentTimeMillis();
        state.setProperty("updated", Long.toString(updated));
        state.setProperty("progress", Float.toString(record.progress));
        state.setProperty("abandoned", Boolean.toString(record.abandoned));
        state.setProperty("sealed", Boolean.toString(record.sealed));
        state.setProperty("preRequested", Boolean.toString(record.preRequested));
        state.setProperty("preApproved", Boolean.toString(record.preApproved));
        if (record.preRequested && !record.abandoned) {
            state.setProperty("prePackage", record.prePackage);
            state.setProperty("preLabel", record.preLabel);
            state.setProperty("preLocale", record.preLocale);
        }
        if (!record.abandoned) {
            state.setProperty("params", Base64.encodeToString(encodeParams(record.params), Base64.NO_WRAP));
            state.setProperty("icon", Base64.encodeToString(record.icon, Base64.NO_WRAP));
        }
        File file = new File(directory(record.id), "session.properties"), pending = new File(file.getPath() + ".tmp");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        state.store(bytes, "Stone package install session");
        if (bytes.size() > MAX_METADATA) throw new IOException("Install session metadata is too large");
        try {
            try (FileOutputStream output = new FileOutputStream(pending)) { bytes.writeTo(output); output.getFD().sync(); }
            Os.rename(pending.getPath(), file.getPath());
            record.updated = updated;
        } finally { pending.delete(); }
    }

    private SessionInfo info(Record record) throws Exception {
        SessionInfo result = SessionInfo.class.getConstructor().newInstance();
        set(result, "sessionId", record.id);
        set(result, "userId", record.user);
        set(result, "installerPackageName", record.owner);
        set(result, "installerAttributionTag", record.attribution);
        set(result, "active", record.opens > 0 || record.running);
        set(result, "sealed", record.sealed);
        set(result, "isCommitted", record.sealed);
        set(result, "isPreapprovalRequested", record.preRequested);
        set(result, "progress", record.progress);
        set(result, "createdMillis", record.created);
        set(result, "updatedMillis", record.updated);
        set(result, "installerUid", record.uid);
        for (String field : new String[]{"mode", "installReason", "installScenario", "sizeBytes", "appPackageName", "appLabel",
                "installLocation", "originatingUri", "originatingUid", "referrerUri", "installFlags", "requireUserAction", "packageSource"})
            set(result, field, get(record.params, field));
        if (record.icon.length != 0) set(result, "appIcon", BitmapFactory.decodeByteArray(record.icon, 0, record.icon.length));
        return result;
    }

    private void abandon(Record record) throws Exception {
        if (record.publishing) throw new SecurityException("Install is already being published");
        if (record.sealed || record.prePending) { end(record, PackageInstaller.STATUS_FAILURE_ABORTED, "Installation cancelled", null); return; }
        record.abandoned = true;
        try { persist(record); } // Retain the ID as a tombstone; it cannot be reused.
        catch (Exception error) { record.abandoned = false; throw error; }
        try {
            for (Object bridge : record.bridges) bridge.getClass().getMethod("forceClose").invoke(bridge);
            File directory = new File(directory(record.id), "files");
            if (!directory(record.id).equals(directory.getCanonicalFile().getParentFile())) throw new SecurityException("Invalid staging directory");
            File[] files = directory.listFiles();
            if (files != null) for (File file : files) {
                if (!directory.getCanonicalFile().equals(file.getCanonicalFile().getParentFile())) throw new SecurityException("Invalid staged file");
                if (!file.delete()) throw new IOException("Could not remove staged install file");
            }
            directory.delete();
        } finally {
            live.remove(record.id);
            notifyCallbacks("onSessionFinished", record.id, false);
        }
    }

    private synchronized void notifyCallbacks(String name, Object... arguments) {
        try {
            Class<?> type = Class.forName("android.content.pm.IPackageInstallerCallback");
            Method selected = null;
            for (Method method : type.getMethods()) if (method.getName().equals(name)) selected = method;
            if (selected == null) throw new NoSuchMethodException(name);
            for (IInterface callback : new ArrayList<>(callbacks.values())) try { selected.invoke(callback, arguments); }
            catch (ReflectiveOperationException | RuntimeException error) { Log.w("StoneInstaller", "Install session callback failed", error); }
        } catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not notify install session callback", error); }
    }

    private static void mutable(Record record) {
        if (record.sealed) throw new SecurityException("Install session is sealed");
    }

    private String transaction(Record record) { return record.owner + "#" + record.user + "#" + record.id; }

    private static void validatePreapproval(String name, String label, String locale) {
        if (name == null || !name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+")
                || label == null || label.isEmpty() || label.length() > 1024 || locale == null || locale.isEmpty()
                || android.os.LocaleList.forLanguageTags(locale).isEmpty())
            throw new IllegalArgumentException("Invalid install preapproval details");
    }

    private void checkStatusReceiver(IntentSender receiver) throws ReflectiveOperationException {
        if (receiver == null) throw new IllegalArgumentException("Missing install status receiver");
        if (apk.applicationInfo.targetSdkVersion >= 35 && (Boolean) IntentSender.class.getMethod("isImmutable").invoke(receiver))
            throw new IllegalArgumentException("Install status receiver must be mutable for target SDK 35+");
        if (context == null) throw new IllegalStateException("Application context is not ready for APK installation");
    }

    private void requestPreapproval(Record record, PackageInstaller.PreapprovalDetails details, IntentSender receiver) throws Exception {
        mutable(record);
        checkStatusReceiver(receiver);
        if (details == null) throw new IllegalArgumentException("Missing preapproval details");
        if (record.preRequested) throw new IllegalStateException("Preapproval was already requested");
        String name = details.getPackageName(), label = details.getLabel().toString(), locale = details.getLocale().toLanguageTag();
        validatePreapproval(name, label, locale);
        String expected = (String) get(record.params, "appPackageName");
        if (expected != null && !expected.equals(name)) throw new IllegalArgumentException("Preapproval package does not match session");
        record.preRequested = true;
        record.prePackage = name; record.preLabel = label; record.preLocale = locale;
        try { persist(record); }
        catch (Exception error) { record.preRequested = false; record.prePackage = record.preLabel = record.preLocale = null; throw error; }
        record.preReceiver = receiver;
        if ((Integer) get(record.params, "mode") != SessionParams.MODE_FULL_INSTALL) {
            sendPreStatus(record, PackageInstaller.STATUS_FAILURE_BLOCKED, "Preapproval requires a full single-APK install", null);
            record.preReceiver = null;
            return;
        }
        record.prePending = record.pending = true;
        record.packageName = name; record.label = label; record.version = null;
        record.previousVersion = null;
        record.nonce = java.util.UUID.randomUUID().toString();
        sendPreStatus(record, PackageInstaller.STATUS_PENDING_USER_ACTION, "User preapproval required", confirmationIntent(record));
    }

    private Intent confirmationIntent(Record record) {
        return new Intent(CONFIRM).setComponent(new ComponentName(record.owner, ArtInstallActivity.class.getName()))
            .putExtra(PackageInstaller.EXTRA_SESSION_ID, record.id).putExtra(NONCE, record.nonce);
    }

    private void commit(Record record, IntentSender receiver, boolean transferred) throws Exception {
        if (transferred) throw new UnsupportedOperationException("Transferred install sessions are not connected yet");
        checkStatusReceiver(receiver);
        if (record.running || record.pending && !record.prePending || record.receiver != null) throw new IllegalStateException("Install session was already committed");
        for (Object bridge : record.bridges) if (!(Boolean) bridge.getClass().getMethod("isClosed").invoke(bridge))
            throw new SecurityException("Close all install write streams before commit");
        boolean sealed = record.sealed;
        record.sealed = true;
        try { persist(record); } catch (Exception error) { record.sealed = sealed; throw error; }
        if (record.prePending) {
            record.prePending = record.pending = false;
            record.nonce = null;
            sendPreStatus(record, PackageInstaller.STATUS_FAILURE_BLOCKED, "Session committed before preapproval completed", null);
            record.preReceiver = null;
        }
        record.receiver = receiver;
        record.running = true;
        notifyCallbacks("onSessionActiveChanged", record.id, true);
        worker(record, () -> prepare(record));
    }

    private void worker(Record record, Runnable work) {
        Thread thread = new Thread(work, "Stone APK commit " + record.id);
        thread.setDaemon(true);
        try { thread.start(); }
        catch (RuntimeException error) {
            record.running = false;
            end(record, PackageInstaller.STATUS_FAILURE, "Could not start install worker", null);
            throw error;
        }
    }

    private void prepare(Record record) {
        ArtPackageStore.Candidate candidate = null;
        try {
            String receipt = packages.receipt(transaction(record));
            if (receipt != null) { end(record, PackageInstaller.STATUS_SUCCESS, "Installation already completed", receipt); return; }
            if ((Integer) get(record.params, "mode") != SessionParams.MODE_FULL_INSTALL)
                throw new ArtPackageStore.Failure(PackageInstaller.STATUS_FAILURE_INCOMPATIBLE, "Only full single-APK install sessions are connected");
            File[] files = new File(directory(record.id), "files").listFiles();
            if (files == null || files.length != 1 || !files[0].isFile())
                throw new ArtPackageStore.Failure(PackageInstaller.STATUS_FAILURE_INVALID, "Expected one complete base APK");
            candidate = packages.prepare(context, files[0].getPath(), (String) get(record.params, "appPackageName"));
            String label = candidate.info.applicationInfo.loadLabel(context.getPackageManager()).toString();
            byte[] confirmationIcon = ArtAppearance.settingsIcon(candidate.info);
            // Snapshot replacement metadata on this worker, before publishing
            // the immutable confirmation. Activity recreation never reads disk.
            android.content.pm.PackageInfo previous = packages.get(candidate.info.packageName,0);
            boolean approved = record.preApproved;
            if (approved && (!record.prePackage.equals(candidate.info.packageName)
                    || !record.preLabel.equals(ArtArchive.labelInLocale(candidate.info, record.preLocale))))
                throw new ArtPackageStore.Failure(PackageInstaller.STATUS_FAILURE_INVALID, "APK does not match the preapproved package and localized label");
            synchronized (this) {
                if (record.abandoned) { candidate.close(); return; }
                record.candidate = candidate;
                record.packageName = candidate.info.packageName;
                record.version = ArtSettingsConfirmation.version(candidate.info);
                record.previousVersion = ArtSettingsConfirmation.version(previous);
                record.label = label;
                record.confirmationIcon = confirmationIcon;
                record.replacing = previous != null;
                record.automatic = !approved && (Integer) get(record.params,"requireUserAction") == SessionParams.USER_ACTION_NOT_REQUIRED
                    && ((Integer) get(record.params,"installFlags") & 0x400 /* INSTALL_FORCE_PERMISSION_PROMPT */) == 0;
                record.progress = Math.max(record.progress, 0.8f);
                notifyCallbacks("onSessionProgressChanged", record.id, record.progress);
                if (approved || record.automatic) { record.running = record.publishing = true; }
                else requireConfirmation(record);
            }
            if (approved || record.automatic) publish(record);
        } catch (Exception error) {
            if (candidate != null) try { candidate.close(); } catch (IOException cleanup) { error.addSuppressed(cleanup); }
            end(record, error instanceof ArtPackageStore.Failure ? ((ArtPackageStore.Failure) error).status : PackageInstaller.STATUS_FAILURE_STORAGE,
                error.getMessage(), null);
            Log.w("StoneInstaller", "Install preparation failed", error);
        }
    }

    // Called with the installer monitor held, after APK preparation or a
    // publication-time automatic-update eligibility check. No disk reads here.
    private void requireConfirmation(Record record) {
        record.automatic = record.publishing = record.running = false;
        record.nonce = java.util.UUID.randomUUID().toString();
        record.pending = true;
        if (record.opens == 0) notifyCallbacks("onSessionActiveChanged", record.id, false);
        sendStatus(record, PackageInstaller.STATUS_PENDING_USER_ACTION, "User confirmation required", confirmationIntent(record));
    }

    synchronized ActivityInfo confirmationInfo(ComponentName component) {
        if (component == null || !apk.applicationInfo.packageName.equals(component.getPackageName())
                || !ArtInstallActivity.class.getName().equals(component.getClassName())) return null;
        ActivityInfo info = new ActivityInfo(apk.activityInfo);
        info.name = ArtInstallActivity.class.getName();
        info.targetActivity = null;
        info.exported = false;
        info.configChanges = 0;
        info.labelRes = 0;
        info.nonLocalizedLabel = ArtText.get(ArtStrings.install_title);
        boolean night = (apk.resources.getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK)
            == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        info.theme = night ? android.R.style.Theme_DeviceDefault : android.R.style.Theme_DeviceDefault_Light;
        return info;
    }

    synchronized ActivityInfo resolveConfirmation(Intent intent) {
        ActivityInfo info = confirmationInfo(intent.getComponent());
        return info != null && confirmation(intent) != null ? info : null;
    }

    static final class Confirmation {
        final int id;
        final byte[] icon;
        final String[] permissions;
        final String nonce, label, packageName, version, previousVersion;
        final boolean preapproval;
        Confirmation(Record record) { id = record.id; nonce = record.nonce; label = record.label; packageName = record.packageName; version = record.version; previousVersion = record.previousVersion; preapproval = record.prePending; icon = preapproval ? new byte[0] : record.confirmationIcon.clone(); permissions = preapproval ? null : ArtSettingsConfirmation.permissions(record.candidate.info); }
    }

    synchronized Confirmation confirmation(Intent intent) {
        if (!CONFIRM.equals(intent.getAction())) return null;
        Record record = live.get(intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1));
        if (record == null || !record.pending || record.abandoned || record.nonce == null || !record.nonce.equals(intent.getStringExtra(NONCE))) return null;
        return new Confirmation(record);
    }

    synchronized void confirm(int id, String nonce, boolean accepted) {
        Record record = live.get(id);
        if (record == null || !record.pending || record.abandoned || record.nonce == null || !record.nonce.equals(nonce)) return;
        if (record.prePending) {
            if (!accepted) { end(record, PackageInstaller.STATUS_FAILURE_ABORTED, "Preapproval declined", record.prePackage); return; }
            record.preApproved = true;
            int status = PackageInstaller.STATUS_SUCCESS;
            String message = "Installation preapproved";
            try { persist(record); }
            catch (Exception error) {
                record.preApproved = false;
                status = PackageInstaller.STATUS_FAILURE_STORAGE;
                message = "Could not persist install preapproval";
                Log.w("StoneInstaller", message, error);
            }
            record.prePending = record.pending = false;
            record.nonce = null;
            sendPreStatus(record, status, message, null);
            record.preReceiver = null;
            return;
        }
        record.pending = false;
        if (!accepted) { end(record, PackageInstaller.STATUS_FAILURE_ABORTED, "Installation cancelled", record.packageName); return; }
        record.publishing = record.running = true;
        notifyCallbacks("onSessionActiveChanged", record.id, true);
        worker(record, () -> publish(record));
    }

    private void publish(Record record) {
        try {
            packages.commit(record.candidate, record.owner, transaction(record), apk.packageInfo, record.automatic,
                ((Integer) get(record.params,"installFlags") & (1 << 25)) != 0);
            if (record.automatic) Log.i("StoneInstaller", "Published automatic update: " + record.packageName);
            end(record, PackageInstaller.STATUS_SUCCESS, "Installation completed", record.packageName);
        } catch (ArtPackageStore.UserActionRequired action) {
            synchronized (this) { if (!record.abandoned) requireConfirmation(record); }
        } catch (Exception error) {
            try {
                String receipt = packages.receipt(transaction(record));
                if (receipt != null) { end(record, PackageInstaller.STATUS_SUCCESS, "Installation completed", receipt); return; }
            } catch (IOException ignored) { }
            end(record, error instanceof ArtPackageStore.Failure ? ((ArtPackageStore.Failure) error).status : PackageInstaller.STATUS_FAILURE_STORAGE,
                error.getMessage(), record.packageName);
            Log.w("StoneInstaller", "Install publication failed", error);
        }
    }

    private synchronized void end(Record record, int status, String message, String packageName) {
        if (record.terminalSent) return;
        record.terminalSent = true;
        record.abandoned = true;
        record.prePending = record.pending = record.running = record.publishing = false;
        if (packageName != null) record.packageName = packageName;
        if (status == PackageInstaller.STATUS_SUCCESS) record.progress = 1;
        boolean persisted = false;
        try { persist(record); persisted = true; }
        catch (Exception error) { Log.w("StoneInstaller", "Could not persist finished session; package receipts protect successful publication", error); }
        try {
            if (record.candidate != null) { record.candidate.close(); record.candidate = null; }
            for (Object bridge : record.bridges) bridge.getClass().getMethod("forceClose").invoke(bridge);
            File files = new File(directory(record.id), "files");
            File[] entries = files.listFiles();
            if (entries != null) for (File file : entries) if (!file.delete()) Log.w("StoneInstaller", "Could not delete completed session file");
            if (files.exists() && !files.delete()) Log.w("StoneInstaller", "Could not delete completed session directory");
        } catch (Exception error) { Log.w("StoneInstaller", "Could not clean completed session", error); }
        notifyCallbacks("onSessionFinished", record.id, status == PackageInstaller.STATUS_SUCCESS);
        sendPreStatus(record, status, message, null);
        record.preReceiver = null;
        sendStatus(record, status, message, null);
        record.receiver = null;
        if (persisted) live.remove(record.id);
    }

    private void sendStatus(Record record, int status, String message, Intent confirmation) {
        sendStatus(record, record.receiver, false, status, message, confirmation);
    }

    private void sendPreStatus(Record record, int status, String message, Intent confirmation) {
        sendStatus(record, record.preReceiver, true, status, message, confirmation);
    }

    private void sendStatus(Record record, IntentSender receiver, boolean preapproval, int status, String message, Intent confirmation) {
        if (receiver == null) return;
        Intent result = new Intent().putExtra(PackageInstaller.EXTRA_SESSION_ID, record.id)
            .putExtra(PackageInstaller.EXTRA_STATUS, status).putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, message)
            .putExtra(PackageInstaller.EXTRA_PRE_APPROVAL, preapproval);
        if (record.packageName != null) result.putExtra(PackageInstaller.EXTRA_PACKAGE_NAME, record.packageName);
        if (confirmation != null) result.putExtra(Intent.EXTRA_INTENT, confirmation);
        try { receiver.sendIntent(context, 0, result, null, null); }
        catch (IntentSender.SendIntentException | RuntimeException error) { Log.w("StoneInstaller", "Install status receiver could not be delivered", error); }
    }

    private final class Handle {
        final Record record;
        boolean closed;
        Handle(Record record) { this.record = record; }
        Object call(Object proxy, Method method, Object[] args) throws Throwable {
            synchronized (ArtPackageInstaller.this) {
                if (method.getName().equals("close")) {
                    if (!closed) {
                        closed = true;
                        if (--record.opens == 0 && !record.abandoned && !record.running) notifyCallbacks("onSessionActiveChanged", record.id, false);
                    }
                    return null;
                }
                if (closed || record.abandoned) throw new SecurityException("Install session is closed or abandoned");
                try {
                    switch (method.getName()) {
                        case "setClientProgress": mutable(record); progress((Float) args[0]); return null;
                        case "addClientProgress": mutable(record); progress(record.progress + (Float) args[0]); return null;
                        case "getNames": {
                            String[] names = files().list();
                            if (names == null) throw new IOException("Cannot list staged files");
                            java.util.Arrays.sort(names);
                            return names;
                        }
                        case "openWrite": mutable(record); return write((String) args[0], (Long) args[1], (Long) args[2]);
                        case "openRead": return ParcelFileDescriptor.open(file((String) args[0]), ParcelFileDescriptor.MODE_READ_ONLY);
                        case "abandon": abandon(record); return null;
                        case "commit": commit(record, (IntentSender) args[0], (Boolean) args[1]); return null;
                        case "requestUserPreapproval": requestPreapproval(record, (PackageInstaller.PreapprovalDetails) args[0], (IntentSender) args[1]); return null;
                        case "isMultiPackage":
                        case "isStaged": return false;
                        case "getParentSessionId": return -1;
                        case "getChildSessionIds": return new int[0];
                        case "getDataLoaderParams": return null;
                        default: throw new UnsupportedOperationException("ART install session: " + method.getName());
                    }
                } catch (IOException error) { throw transport(error); }
                catch (android.system.ErrnoException error) { throw transport(new IOException(error)); }
                catch (InvocationTargetException error) { throw error.getCause(); }
            }
        }

        private void progress(float value) throws Exception {
            if (!Float.isFinite(value)) throw new IllegalArgumentException("Invalid install progress");
            float previous = record.progress;
            record.progress = Math.max(0, Math.min(1, value));
            try { persist(record); }
            catch (Exception error) { record.progress = previous; throw error; }
            notifyCallbacks("onSessionProgressChanged", record.id, record.progress);
        }

        private File files() throws IOException {
            File session = directory(record.id), files = new File(session, "files").getCanonicalFile();
            if (!session.equals(files.getParentFile())) throw new SecurityException("Staging directory escaped the session");
            if (!files.isDirectory() && !files.mkdir()) throw new IOException("Cannot create install staging directory");
            return files;
        }

        private File file(String name) throws IOException {
            if (name == null || name.isEmpty() || name.length() > 255 || name.equals(".") || name.equals("..")
                    || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf(':') >= 0 || name.indexOf('\0') >= 0)
                throw new IllegalArgumentException("Invalid staged file name");
            File parent = files(), file = new File(parent, name).getCanonicalFile();
            if (!parent.equals(file.getParentFile())) throw new SecurityException("Staged file escaped the session");
            return file;
        }

        private ParcelFileDescriptor write(String name, long offset, long length) throws Exception {
            if (offset < 0 || length < -1) throw new IllegalArgumentException("Invalid staged file offset or length");
            if (PackageInstaller.class.getField("ENABLE_REVOCABLE_FD").getBoolean(null))
                throw new UnsupportedOperationException("Revocable install descriptors require an Android FUSE backend");
            ParcelFileDescriptor target = ParcelFileDescriptor.open(file(name), ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_READ_WRITE);
            Object bridge = null;
            try {
                if (length > Os.fstat(target.getFileDescriptor()).st_size) Os.ftruncate(target.getFileDescriptor(), length);
                Os.lseek(target.getFileDescriptor(), offset, OsConstants.SEEK_SET);
                Class<?> type = Class.forName("android.os.FileBridge");
                bridge = type.getConstructor().newInstance();
                type.getMethod("setTargetFile", ParcelFileDescriptor.class).invoke(bridge, target);
                ParcelFileDescriptor client = (ParcelFileDescriptor) type.getMethod("getClientSocket").invoke(bridge);
                for (java.util.Iterator<Object> next = record.bridges.iterator(); next.hasNext();) {
                    Object previous = next.next();
                    if ((Boolean) type.getMethod("isClosed").invoke(previous)) next.remove();
                }
                record.bridges.add(bridge);
                ((Thread) bridge).setName("Stone install session " + record.id);
                ((Thread) bridge).setDaemon(true);
                ((Thread) bridge).start();
                return client;
            } catch (Exception error) {
                target.close();
                if (bridge != null) bridge.getClass().getMethod("forceClose").invoke(bridge);
                throw error;
            }
        }
    }

    private static final class Record {
        final int id, user, uid;
        final String owner, attribution;
        final SessionParams params;
        byte[] icon;
        byte[] confirmationIcon = new byte[0];
        float progress;
        long created = System.currentTimeMillis(), updated = created;
        int opens;
        boolean abandoned, sealed, running, pending, publishing, terminalSent, replacing, automatic;
        boolean preRequested, preApproved, prePending;
        IntentSender receiver, preReceiver;
        ArtPackageStore.Candidate candidate;
        String nonce, label, packageName, version, previousVersion;
        String prePackage, preLabel, preLocale;
        final List<Object> bridges = new ArrayList<>();
        Record(int id, int user, int uid, String owner, String attribution, SessionParams params, byte[] icon) {
            this.id = id; this.user = user; this.uid = uid; this.owner = owner; this.attribution = attribution; this.params = params; this.icon = icon;
        }
    }

    private static byte[] encodeParams(SessionParams params) {
        Parcel parcel = Parcel.obtain();
        try { params.writeToParcel(parcel, 0); return parcel.marshall(); }
        finally { parcel.recycle(); }
    }

    private static SessionParams decodeParams(byte[] bytes) {
        Parcel parcel = Parcel.obtain();
        try { parcel.unmarshall(bytes, 0, bytes.length); parcel.setDataPosition(0); return SessionParams.CREATOR.createFromParcel(parcel); }
        finally { parcel.recycle(); }
    }

    private static byte[] encodeIcon(Bitmap icon) throws IOException {
        if (icon == null) return new byte[0];
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        if (!icon.compress(Bitmap.CompressFormat.PNG, 100, output)) throw new IOException("Could not encode install session icon");
        return output.toByteArray();
    }

    private static Object get(Object object, String name) throws ReflectiveOperationException { return object.getClass().getField(name).get(object); }
    private static void set(Object object, String name, Object value) throws ReflectiveOperationException { object.getClass().getField(name).set(object, value); }
    private static RuntimeException transport(IOException error) throws ReflectiveOperationException {
        return (RuntimeException) Class.forName("android.os.ParcelableException").getConstructor(Throwable.class).newInstance(error);
    }
}
